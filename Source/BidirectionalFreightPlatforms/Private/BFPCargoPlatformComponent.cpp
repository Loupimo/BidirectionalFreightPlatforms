// Copyright Loupimo. All Rights Reserved.

#include "BFPCargoPlatformComponent.h"
#include "FGInventoryComponent.h"
#include "Net/UnrealNetwork.h"

UBFPLoadInventoryComponent::UBFPLoadInventoryComponent()
{
	// Replicate from the constructor (the reliable path) so the load buffer reaches clients in MP.
	SetIsReplicatedByDefault( true );
}

namespace
{
	UFGInventoryComponent* FindInvByName( const AActor* Actor, const TCHAR* Name )
	{
		if ( !Actor )
		{
			return nullptr;
		}
		TInlineComponentArray<UFGInventoryComponent*> Comps;
		Actor->GetComponents( Comps );
		for ( UFGInventoryComponent* C : Comps )
		{
			if ( C && C->GetName() == Name )
			{
				return C;
			}
		}
		return nullptr;
	}
}

UBFPCargoPlatformComponent::UBFPCargoPlatformComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault( true );
	bInitialized = 0;
	mStationMode = EBFPStationMode::Both;
	mLoadRate = 0.f;
	mUnloadRate = 0.f;
}

void UBFPCargoPlatformComponent::GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( UBFPCargoPlatformComponent, mStationMode );
	DOREPLIFETIME( UBFPCargoPlatformComponent, mLoadRate );
	DOREPLIFETIME( UBFPCargoPlatformComponent, mUnloadRate );
	DOREPLIFETIME( UBFPCargoPlatformComponent, mLoadInventory );
}

bool UBFPCargoPlatformComponent::ShouldSave_Implementation() const
{
	return true;
}

bool UBFPCargoPlatformComponent::NeedTransform_Implementation()
{
	return false;
}

void UBFPCargoPlatformComponent::PublishTransferRates( float NewLoadRate, float NewUnloadRate )
{
	mLoadRate = NewLoadRate;
	mUnloadRate = NewUnloadRate;
	// Authority broadcasts locally (single-player / listen-server host); remote clients fire via OnRep.
	OnTransferRateUpdated.Broadcast( mLoadRate, mUnloadRate );
}

void UBFPCargoPlatformComponent::OnRep_TransferRate()
{
	OnTransferRateUpdated.Broadcast( mLoadRate, mUnloadRate );
}

void UBFPCargoPlatformComponent::SetStationMode( EBFPStationMode Mode )
{
	// Authority-side apply. In MP, clients route through UBFPInteractProxyComponent (a client cannot mutate
	// a buildable directly), which calls this on the server; the result replicates back via mStationMode.
	mStationMode = Mode;
	bInitialized = 1;

	// Load is involved (Load or Both): make sure the load buffer exists right away for the UI.
	if ( Mode == EBFPStationMode::Load || Mode == EBFPStationMode::Both )
	{
		EnsureLoadInventory();
	}
}

void UBFPCargoPlatformComponent::SetLoadUnloadEnabled( bool bLoadEnabled, bool bUnloadEnabled )
{
	EBFPStationMode Mode;
	if ( bLoadEnabled && bUnloadEnabled ) { Mode = EBFPStationMode::Both; }
	else if ( bLoadEnabled )              { Mode = EBFPStationMode::Load; }
	else                                  { Mode = EBFPStationMode::Unload; } // (off,off) -> Unload
	SetStationMode( Mode );
}

void UBFPCargoPlatformComponent::SetBidirectionalEnabled( bool bEnabled )
{
	SetStationMode( bEnabled ? EBFPStationMode::Both : EBFPStationMode::Unload );
}

UFGInventoryComponent* UBFPCargoPlatformComponent::GetLoadInventory() const
{
	if ( mLoadInventory )
	{
		return mLoadInventory;
	}
	// Client-safe fallback: find by CLASS, not name. Replicated components get generated names on clients,
	// so FindInvByName("BFP_LoadInventory") would fail there.
	return GetOwner() ? GetOwner()->FindComponentByClass<UBFPLoadInventoryComponent>() : nullptr;
}

UFGInventoryComponent* UBFPCargoPlatformComponent::EnsureLoadInventory()
{
	AActor* Owner = GetOwner();
	UFGInventoryComponent* V = FindInvByName( Owner, TEXT( "inventory" ) );

	UFGInventoryComponent* L = mLoadInventory ? mLoadInventory.Get() : FindInvByName( Owner, TEXT( "BFP_LoadInventory" ) );

	// Only the authority creates the component; clients pick up the replicated one by name.
	if ( !L && Owner && Owner->HasAuthority() )
	{
		L = NewObject<UBFPLoadInventoryComponent>( Owner, TEXT( "BFP_LoadInventory" ) );
		if ( L )
		{
			L->RegisterComponent(); // replication flag is set in UBFPLoadInventoryComponent's constructor
			if ( V )
			{
				// Clone capacity / slot sizes / filters. CopyFromOtherComponent ALSO copies the items, so for
				// a vanilla station that already held cargo this would DUPLICATE it. De-duplicate by keeping
				// the items only on the inventory the mode actually uses, and emptying the other:
				L->CopyFromOtherComponent( V );
				if ( mStationMode == EBFPStationMode::Load )
				{
					// Loading reads from the load buffer -> the staged items belong there; clear the unload buffer.
					V->Empty();
				}
				else
				{
					// Unload / Both -> the load buffer starts empty; items (if any) stay in the unload buffer.
					L->Empty();
				}
			}
		}
	}

	// Keep the load buffer exactly the same size as the vanilla (unload) inventory. This heals buffers
	// created by older builds at a wrong/larger size and persisted in the save. Authority only; the
	// Resize replicates to clients. (Resize keeps items in the surviving low-index slots.)
	if ( L && V && Owner && Owner->HasAuthority() && L->GetSizeLinear() != V->GetSizeLinear() )
	{
		UE_LOG( LogTemp, Warning, TEXT( "[BFP] Resizing load buffer on %s from %d to %d to match the unload inventory" ),
			*Owner->GetName(), L->GetSizeLinear(), V->GetSizeLinear() );
		L->Resize( V->GetSizeLinear() );
	}

	mLoadInventory = L;
	// Per-slot sizes (fluid platforms set a large ARBITRARY slot size that CopyFromOtherComponent does NOT
	// clone, nor does it replicate). Runs on the server here; the client redoes it in OnRep_LoadInventory.
	ReconcileLoadBufferCapacity();
	return L;
}

void UBFPCargoPlatformComponent::OnRep_LoadInventory()
{
	// Client: the load buffer pointer just arrived; copy the vanilla inventory's slot sizes onto it so the
	// fluid capacity displays correctly (the arbitrary slot size is not replicated).
	ReconcileLoadBufferCapacity();
}

void UBFPCargoPlatformComponent::ReconcileLoadBufferCapacity()
{
	UFGInventoryComponent* L = mLoadInventory.Get();
	UFGInventoryComponent* V = FindInvByName( GetOwner(), TEXT( "inventory" ) );
	if ( !L || !V )
	{
		return;
	}
	const int32 Num = FMath::Min( L->GetSizeLinear(), V->GetSizeLinear() );
	for ( int32 i = 0; i < Num; ++i )
	{
		const int32 VSize = V->GetSlotSize( i );
		if ( L->GetSlotSize( i ) != VSize )
		{
			L->AddArbitrarySlotSize( i, VSize ); // copy the (un-cloned, un-replicated) arbitrary fluid capacity
		}
	}
}

// Copyright Loupimo. All Rights Reserved.

#include "BFPCargoPlatformComponent.h"
#include "FGInventoryComponent.h"
#include "Net/UnrealNetwork.h"

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
}

bool UBFPCargoPlatformComponent::ShouldSave_Implementation() const
{
	return true;
}

bool UBFPCargoPlatformComponent::NeedTransform_Implementation()
{
	return false;
}

void UBFPCargoPlatformComponent::SetStationMode( EBFPStationMode Mode )
{
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
	return FindInvByName( GetOwner(), TEXT( "BFP_LoadInventory" ) );
}

UFGInventoryComponent* UBFPCargoPlatformComponent::EnsureLoadInventory()
{
	AActor* Owner = GetOwner();
	UFGInventoryComponent* V = FindInvByName( Owner, TEXT( "inventory" ) );

	UFGInventoryComponent* L = mLoadInventory ? mLoadInventory.Get() : FindInvByName( Owner, TEXT( "BFP_LoadInventory" ) );

	// Only the authority creates the component; clients pick up the replicated one by name.
	if ( !L && Owner && Owner->HasAuthority() )
	{
		L = NewObject<UFGInventoryComponent>( Owner, TEXT( "BFP_LoadInventory" ) );
		if ( L )
		{
			L->RegisterComponent();
			// Clone the vanilla inventory's capacity / slot sizes / filters (it is empty on a fresh platform).
			if ( V )
			{
				L->CopyFromOtherComponent( V );
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
	return L;
}

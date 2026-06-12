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
	mBidirectionalEnabled = 0;
}

void UBFPCargoPlatformComponent::GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( UBFPCargoPlatformComponent, mBidirectionalEnabled );
}

bool UBFPCargoPlatformComponent::ShouldSave_Implementation() const
{
	return true;
}

bool UBFPCargoPlatformComponent::NeedTransform_Implementation()
{
	return false;
}

void UBFPCargoPlatformComponent::SetBidirectionalEnabled( bool bEnabled )
{
	mBidirectionalEnabled = bEnabled ? 1 : 0;
	bInitialized = 1;

	// Enabling: make sure the load buffer exists right away so the UI has something to show.
	if ( mBidirectionalEnabled )
	{
		EnsureLoadInventory();
	}
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
	if ( mLoadInventory )
	{
		return mLoadInventory;
	}

	AActor* Owner = GetOwner();
	UFGInventoryComponent* L = FindInvByName( Owner, TEXT( "BFP_LoadInventory" ) );

	// Only the authority creates the component; clients pick up the replicated one by name.
	if ( !L && Owner && Owner->HasAuthority() )
	{
		L = NewObject<UFGInventoryComponent>( Owner, TEXT( "BFP_LoadInventory" ) );
		if ( L )
		{
			L->RegisterComponent();
			// Clone the vanilla inventory's capacity / slot sizes / filters (it is empty on a fresh platform).
			if ( UFGInventoryComponent* V = FindInvByName( Owner, TEXT( "inventory" ) ) )
			{
				L->CopyFromOtherComponent( V );
			}
		}
	}

	mLoadInventory = L;
	return L;
}

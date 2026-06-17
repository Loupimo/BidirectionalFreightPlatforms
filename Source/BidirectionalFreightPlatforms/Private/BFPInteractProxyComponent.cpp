// Copyright Loupimo. All Rights Reserved.

#include "BFPInteractProxyComponent.h"
#include "BFPBlueprintLibrary.h" // FlushStationPipes (server-side pipe flush)
#include "Buildables/FGBuildableTrainPlatformCargo.h"

UBFPInteractProxyComponent::UBFPInteractProxyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	// Replicated so the owning client gets a replica it can fire Server RPCs from.
	SetIsReplicatedByDefault( true );
}

namespace
{
	EBFPStationMode BFP_ModeFromSwitches( bool bLoad, bool bUnload )
	{
		if ( bLoad && bUnload ) { return EBFPStationMode::Both; }
		if ( bLoad )            { return EBFPStationMode::Load; }
		return EBFPStationMode::Unload; // (off,off) -> Unload
	}
}

void UBFPInteractProxyComponent::RequestSetStationMode( UBFPCargoPlatformComponent* TargetComponent, EBFPStationMode Mode )
{
	if ( !TargetComponent )
	{
		return;
	}
	if ( GetOwner() && !GetOwner()->HasAuthority() )
	{
		Server_SetStationMode( TargetComponent, Mode ); // client -> server (we own the character)
		return;
	}
	TargetComponent->SetStationMode( Mode );
}

void UBFPInteractProxyComponent::Server_SetStationMode_Implementation( UBFPCargoPlatformComponent* TargetComponent, EBFPStationMode Mode )
{
	if ( TargetComponent )
	{
		TargetComponent->SetStationMode( Mode );
	}
}

void UBFPInteractProxyComponent::RequestSetLoadUnload( UBFPCargoPlatformComponent* TargetComponent, bool bLoadEnabled, bool bUnloadEnabled )
{
	RequestSetStationMode( TargetComponent, BFP_ModeFromSwitches( bLoadEnabled, bUnloadEnabled ) );
}

void UBFPInteractProxyComponent::RequestFlushStationPipes( AFGBuildableTrainPlatformCargo* Platform, bool bInput )
{
	if ( !Platform )
	{
		return;
	}
	if ( GetOwner() && !GetOwner()->HasAuthority() )
	{
		Server_FlushStationPipes( Platform, bInput ); // client -> server
		return;
	}
	UBFPBlueprintLibrary::FlushStationPipes( Platform, bInput );
}

void UBFPInteractProxyComponent::Server_FlushStationPipes_Implementation( AFGBuildableTrainPlatformCargo* Platform, bool bInput )
{
	if ( Platform )
	{
		UBFPBlueprintLibrary::FlushStationPipes( Platform, bInput );
	}
}

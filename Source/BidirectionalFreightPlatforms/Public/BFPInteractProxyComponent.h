// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BFPCargoPlatformComponent.h" // EBFPStationMode
#include "BFPInteractProxyComponent.generated.h"

class AFGBuildableTrainPlatformCargo;

/**
 * Added to the player character (which the owning client controls) so the station UI can route actions
 * to the server. A Server RPC on a buildable's OWN component is dropped because the client is not the
 * buildable's net-owner; routing through this player-owned component is the reliable MP pattern. The
 * server-side handlers then apply the action to the target platform/component.
 */
UCLASS( ClassGroup = ( Custom ), meta = ( BlueprintSpawnableComponent ) )
class BIDIRECTIONALFREIGHTPLATFORMS_API UBFPInteractProxyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBFPInteractProxyComponent();

	/** Set a station's mode (routes client -> server). Pass the platform's UBFPCargoPlatformComponent. */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void RequestSetStationMode( UBFPCargoPlatformComponent* TargetComponent, EBFPStationMode Mode );

	/** Set a station's mode from two UI switches (routes client -> server). */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void RequestSetLoadUnload( UBFPCargoPlatformComponent* TargetComponent, bool bLoadEnabled, bool bUnloadEnabled );

	/** Flush a fluid platform's input (bInput=true) or output pipe network (routes client -> server). */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void RequestFlushStationPipes( AFGBuildableTrainPlatformCargo* Platform, bool bInput );

private:
	UFUNCTION( Server, Reliable )
	void Server_SetStationMode( UBFPCargoPlatformComponent* TargetComponent, EBFPStationMode Mode );

	UFUNCTION( Server, Reliable )
	void Server_FlushStationPipes( AFGBuildableTrainPlatformCargo* Platform, bool bInput );
};

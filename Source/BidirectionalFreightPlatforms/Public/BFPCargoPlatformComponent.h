// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FGSaveInterface.h"
#include "BFPCargoPlatformComponent.generated.h"

/**
 * Saved + replicated marker component attached at runtime to a freight cargo platform. Holds the
 * per-platform "bidirectional" toggle (load AND unload the same wagon in one stop). Persisted by the
 * Satisfactory save system (implements IFGSaveInterface) and found-or-created from our hooks.
 */
UCLASS( ClassGroup = ( Custom ), meta = ( BlueprintSpawnableComponent ) )
class BIDIRECTIONALFREIGHTPLATFORMS_API UBFPCargoPlatformComponent : public UActorComponent, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	UBFPCargoPlatformComponent();

	virtual void GetLifetimeReplicatedProps( TArray<FLifetimeProperty>& OutLifetimeProps ) const override;

	// Begin IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override;
	virtual bool NeedTransform_Implementation() override;
	virtual void PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion ) override {}
	virtual void PostSaveGame_Implementation( int32 saveVersion, int32 gameVersion ) override {}
	virtual void PreLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override {}
	virtual void PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override {}
	virtual void GatherDependencies_Implementation( TArray<UObject*>& out_dependentObjects ) override {}
	// End IFGSaveInterface

	/** Whether this platform should load AND unload the same wagon in one stop. */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	bool IsBidirectionalEnabled() const { return mBidirectionalEnabled != 0; }

	/** Set the toggle (call from the station UI). Authoritative; replicates to clients. */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void SetBidirectionalEnabled( bool bEnabled );

	/**
	 * The LOAD buffer inventory (input belts -> wagon), for the "incoming" panel of the station UI.
	 * Created on demand (server) by cloning the vanilla inventory; safe to call from the widget.
	 */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	class UFGInventoryComponent* GetLoadInventory() const;

	/** Server: create the load buffer (cloning the vanilla inventory) if it does not exist yet. */
	class UFGInventoryComponent* EnsureLoadInventory();

	/** Cached load buffer (also a registered component named "BFP_LoadInventory" on the owner). */
	UPROPERTY()
	TObjectPtr<class UFGInventoryComponent> mLoadInventory;

	/** True once the new-vs-loaded default has been resolved; saved so it is resolved only once. */
	UPROPERTY( SaveGame )
	uint8 bInitialized : 1;

	/** Whether this platform loads AND unloads the same wagon in one stop. */
	UPROPERTY( SaveGame, Replicated )
	uint8 mBidirectionalEnabled : 1;
};

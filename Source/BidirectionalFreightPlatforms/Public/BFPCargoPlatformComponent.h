// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FGSaveInterface.h"
#include "BFPCargoPlatformComponent.generated.h"

/** What a freight platform does for a docked wagon. */
UENUM( BlueprintType )
enum class EBFPStationMode : uint8
{
	Unload	UMETA( DisplayName = "Unload only" ),
	Load	UMETA( DisplayName = "Load only" ),
	Both	UMETA( DisplayName = "Load + Unload (bidirectional)" )
};

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

	/** The station mode: Unload only / Load only / Both (bidirectional). */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	EBFPStationMode GetStationMode() const { return mStationMode; }

	/** Whether this platform loads AND unloads the same wagon in one stop (mode == Both). */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	bool IsBidirectionalEnabled() const { return mStationMode == EBFPStationMode::Both; }

	/** Set the station mode (call from the station UI). Authoritative; replicates to clients. */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void SetStationMode( EBFPStationMode Mode );

	/**
	 * Convenience for two on/off toggles in the UI (a Load switch + an Unload switch):
	 * (on,on) => Both, (on,off) => Load, (off,on) => Unload, (off,off) clamps to Unload.
	 */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	void SetLoadUnloadEnabled( bool bLoadEnabled, bool bUnloadEnabled );

	/** Back-compat: true => Both, false => Unload only. */
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

	/** Current-stop wagon transfer rates (items/min), computed by our hooks. 0 between stops. */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	float GetLoadRate() const { return mLoadRate; }

	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	float GetUnloadRate() const { return mUnloadRate; }

	/** Setters used by the native hooks (authority); the values replicate to clients for the UI. */
	void SetLoadRate( float Rate ) { mLoadRate = Rate; }
	void SetUnloadRate( float Rate ) { mUnloadRate = Rate; }

	/** Cached load buffer (also a registered component named "BFP_LoadInventory" on the owner). */
	UPROPERTY()
	TObjectPtr<class UFGInventoryComponent> mLoadInventory;

	/** True once the new-vs-loaded default has been resolved; saved so it is resolved only once. */
	UPROPERTY( SaveGame )
	uint8 bInitialized : 1;

	/** What this platform does for a docked wagon (Unload / Load / Both). */
	UPROPERTY( SaveGame, Replicated )
	EBFPStationMode mStationMode;

	/** Items/min loaded into the wagon for the current stop (transient, replicated, 0 between stops). */
	UPROPERTY( Replicated )
	float mLoadRate;

	/** Items/min unloaded from the wagon for the current stop (transient, replicated, 0 between stops). */
	UPROPERTY( Replicated )
	float mUnloadRate;
};

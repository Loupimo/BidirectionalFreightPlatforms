// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FGSaveInterface.h"
#include "FGInventoryComponent.h"
#include "BFPCargoPlatformComponent.generated.h"

/**
 * The LOAD buffer inventory. A dedicated subclass so it can set replication in its CONSTRUCTOR
 * (SetIsReplicatedByDefault) — calling SetIsReplicated() at runtime on a plain UFGInventoryComponent did
 * NOT reliably replicate the buffer to clients in MP, while the toggle component (which uses the ctor flag)
 * does replicate. Same pattern here so the load panel shows up on clients.
 */
UCLASS()
class BIDIRECTIONALFREIGHTPLATFORMS_API UBFPLoadInventoryComponent : public UFGInventoryComponent
{
	GENERATED_BODY()
public:
	UBFPLoadInventoryComponent();
};

/** What a freight platform does for a docked wagon. */
UENUM( BlueprintType )
enum class EBFPStationMode : uint8
{
	Unload	UMETA( DisplayName = "Unload only" ),
	Load	UMETA( DisplayName = "Load only" ),
	Both	UMETA( DisplayName = "Load + Unload (bidirectional)" )
};

/** Fired at train departure with the last stop's transfer rates (items/min). Bind from the station UI. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams( FBFPOnTransferRateUpdated, float, LoadRate, float, UnloadRate );

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

	/** Last finished stop's wagon transfer rates (items/min), held until the next train. */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	float GetLoadRate() const { return mLoadRate; }

	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	float GetUnloadRate() const { return mUnloadRate; }

	/**
	 * Broadcast at train departure (whether or not anything was transferred -> 0 if nothing moved),
	 * carrying the last stop's rates. Bind from the station UI for event-driven updates (no tick needed).
	 */
	UPROPERTY( BlueprintAssignable, Category = "BidirectionalFreightPlatforms" )
	FBFPOnTransferRateUpdated OnTransferRateUpdated;

	/** Authority: set both rates and fire OnTransferRateUpdated (called at undock by the hooks). */
	void PublishTransferRates( float NewLoadRate, float NewUnloadRate );

	/** The load buffer. REPLICATED so clients get the pointer directly — looking it up by name fails on
	 *  clients (replicated components get generated names, not "BFP_LoadInventory"). OnRep so the client can
	 *  copy the vanilla inventory's (non-replicated) arbitrary slot size onto it for correct capacity display. */
	UPROPERTY( ReplicatedUsing = OnRep_LoadInventory )
	TObjectPtr<class UFGInventoryComponent> mLoadInventory;

	/** True once the new-vs-loaded default has been resolved; saved so it is resolved only once. */
	UPROPERTY( SaveGame )
	uint8 bInitialized : 1;

	/** What this platform does for a docked wagon (Unload / Load / Both). */
	UPROPERTY( SaveGame, Replicated )
	EBFPStationMode mStationMode;

	/** Items/min loaded into the wagon at the last stop (replicated; held until the next train). */
	UPROPERTY( ReplicatedUsing = OnRep_TransferRate )
	float mLoadRate;

	/** Items/min unloaded from the wagon at the last stop (replicated; held until the next train). */
	UPROPERTY( ReplicatedUsing = OnRep_TransferRate )
	float mUnloadRate;

	/** Client-side: fire OnTransferRateUpdated when a replicated rate arrives. */
	UFUNCTION()
	void OnRep_TransferRate();

	/** Client-side: when the load buffer arrives, copy the vanilla inventory's slot sizes onto it. */
	UFUNCTION()
	void OnRep_LoadInventory();

	/** Copy each slot's effective size from the vanilla inventory onto the load buffer (fluid capacity). */
	void ReconcileLoadBufferCapacity();
};

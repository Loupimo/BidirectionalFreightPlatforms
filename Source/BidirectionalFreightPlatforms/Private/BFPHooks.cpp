// Copyright Loupimo. All Rights Reserved.

#include "BFPHooks.h"

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Buildables/FGBuildableTrainPlatform.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "FGFreightWagon.h"            // AFGFreightWagon, EFreightCargoType (pulls in FGRailroadVehicle.h)
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"

#include "BFPCargoPlatformComponent.h"
#include "BFPBlueprintLibrary.h"

#include "Patching/NativeHookManager.h"

DEFINE_LOG_CATEGORY( LogBFP );

namespace
{
	const TCHAR* DockingStatusToString( ETrainPlatformDockingStatus Status )
	{
		switch ( Status )
		{
		case ETrainPlatformDockingStatus::ETPDS_None:                     return TEXT( "None" );
		case ETrainPlatformDockingStatus::ETPDS_WaitingToStart:           return TEXT( "WaitingToStart" );
		case ETrainPlatformDockingStatus::ETPDS_Loading:                  return TEXT( "Loading" );
		case ETrainPlatformDockingStatus::ETPDS_Unloading:                return TEXT( "Unloading" );
		case ETrainPlatformDockingStatus::ETPDS_WaitingForTransfer:       return TEXT( "WaitingForTransfer" );
		case ETrainPlatformDockingStatus::ETPDS_Complete:                 return TEXT( "Complete" );
		case ETrainPlatformDockingStatus::ETPDS_WaitForTransferCondition: return TEXT( "WaitForTransferCondition" );
		case ETrainPlatformDockingStatus::ETPDS_IdleWaitForTime:          return TEXT( "IdleWaitForTime" );
		default:                                                          return TEXT( "Unknown" );
		}
	}

	/**
	 * Per-platform state.
	 *
	 * Design invariant: the platform's vanilla mInventory ALWAYS rests on the UNLOAD buffer. We only
	 * point it at the LOAD buffer transiently, bracketed inside individual function calls (input
	 * collect, the load-pass dock-sequence evaluation, and the load-pass Factory_Tick that performs
	 * the transfer). It is never left on the load buffer across ticks, so a SaveGame (which happens
	 * between frames) always records mInventory = unload buffer. This is what keeps the two buffers
	 * from collapsing into one across save/reload.
	 */
	struct FBFPPlatform
	{
		/** Vanilla inventory (component "inventory"): UNLOAD buffer (wagon -> here -> output belts). */
		TWeakObjectPtr<UFGInventoryComponent> UnloadInventory;
		/** Our added inventory (component "BFP_LoadInventory"): LOAD buffer (input belts -> here -> wagon). */
		TWeakObjectPtr<UFGInventoryComponent> LoadInventory;

		/** Saved mInventory across the various transient swaps (one per bracket site, never nested). */
		TWeakObjectPtr<UFGInventoryComponent> CollectSaved;   // solid input belts
		TWeakObjectPtr<UFGInventoryComponent> PipeInputSaved; // fluid input pipes
		TWeakObjectPtr<UFGInventoryComponent> TickSaved;
		TWeakObjectPtr<UFGInventoryComponent> SeqSaved;

		/** A dock is in progress and we are orchestrating it. */
		bool bDockActive = false;
		/** The load pass is running (mInventory should be the load buffer during bracketed calls).
		 *  For Load-only mode this is true for the whole dock; for Both it flips on after the unload pass. */
		bool bLoadPass = false;
		/** Both mode only: after the unload pass completes, rewind and run a second (load) pass. */
		bool bTwoPass = false;
		/** The platform's player-configured load mode, restored when the dock ends. */
		bool bOriginalLoadMode = false;

		/** Cached per-platform toggle component (saved + replicated bidirectional flag). */
		TWeakObjectPtr<UBFPCargoPlatformComponent> ToggleComp;

		/** Diagnostics: logged once when each input-redirect hook first fires. */
		bool bLoggedCollectFired = false;
		bool bLoggedPipeFired = false;

		/** Wagon-transfer-rate sampling for the current stop (-1 = no baseline / no wagon docked). */
		int32 LastWagonCount = -1;
		/** Items moved out of the wagon (unload) and into the wagon (load) so far this stop. */
		int32 UnloadMoved = 0;
		int32 LoadMoved = 0;
	};

	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FBFPPlatform> GPlatforms;

	/** Solid (FCT_Standard) or fluid (FCT_Liquid) cargo platform — both use the same two-pass machinery
	 *  (only the input-redirect hook differs: belts use Factory_CollectInput, pipes use Factory_PullPipeInput). */
	bool IsSupportedPlatform( AFGBuildableTrainPlatformCargo* Platform )
	{
		if ( !Platform )
		{
			return false;
		}
		const EFreightCargoType Type = Platform->GetmFreightCargoType();
		return Type == EFreightCargoType::FCT_Standard || Type == EFreightCargoType::FCT_Liquid;
	}

	/** Find-or-create (and cache) the saved toggle component. Does NOT resolve the default yet. */
	UBFPCargoPlatformComponent* EnsureToggleComponent( AFGBuildableTrainPlatformCargo* Platform )
	{
		FBFPPlatform& P = GPlatforms.FindOrAdd( Platform );
		if ( !P.ToggleComp.IsValid() )
		{
			UBFPCargoPlatformComponent* C = Platform->FindComponentByClass<UBFPCargoPlatformComponent>();
			if ( !C )
			{
				C = NewObject<UBFPCargoPlatformComponent>( Platform, TEXT( "BFP_ToggleComponent" ) );
				if ( C )
				{
					C->RegisterComponent();
				}
			}
			P.ToggleComp = C;
		}
		return P.ToggleComp.Get();
	}

	/**
	 * Resolve the station mode default ONCE, at BeginPlay (a safe context, unlike a PostLoadGame hook
	 * which crashed dereferencing the object mid-deserialization). A platform whose BeginPlay runs at
	 * world time ~0 was loaded from the save -> keep its current direction (Load/Unload, don't silently
	 * make an established station bidirectional). One placed later (during play) is new -> default to Both.
	 * A platform whose toggle came from a (mod) save arrives with bInitialized already set -> untouched.
	 */
	void ResolveStationModeOnBeginPlay( AFGBuildableTrainPlatformCargo* Platform, UBFPCargoPlatformComponent* C )
	{
		if ( !C || C->bInitialized )
		{
			return;
		}

		const UWorld* World = Platform->GetWorld();
		const float WorldTime = World ? World->GetTimeSeconds() : 0.f;
		const bool bLoaded = WorldTime < 1.0f; // BeginPlay before the first tick => loaded from save
		const EBFPStationMode Mode = bLoaded
			? ( Platform->GetIsInLoadMode() ? EBFPStationMode::Load : EBFPStationMode::Unload )
			: EBFPStationMode::Both;
		C->SetStationMode( Mode ); // sets mode + bInitialized + ensures the load buffer when loading is involved
		UE_LOG( LogBFP, Verbose, TEXT( "Station mode init on %s: %d (%s, t=%.2f)" ),
			*Platform->GetName(), static_cast<int32>( Mode ),
			bLoaded ? TEXT( "loaded" ) : TEXT( "new->both" ), WorldTime );
	}

	/** Station mode of a standard platform (resolved at BeginPlay; conservative fallback if not yet). */
	EBFPStationMode ResolveStationMode( AFGBuildableTrainPlatformCargo* Platform )
	{
		UBFPCargoPlatformComponent* C = EnsureToggleComponent( Platform );
		if ( !C )
		{
			return EBFPStationMode::Unload;
		}
		// Safety net if a hook somehow runs before BeginPlay resolved it: keep the current direction
		// rather than forcing bidirectional on an established platform.
		if ( !C->bInitialized )
		{
			C->SetStationMode( Platform->GetIsInLoadMode() ? EBFPStationMode::Load : EBFPStationMode::Unload );
		}
		return C->GetStationMode();
	}

	/** Find an inventory component on the actor by its exact name (robust against mInventory pollution). */
	UFGInventoryComponent* FindInventoryByName( AActor* Actor, const TCHAR* Name )
	{
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

	/** Resolve (and cache) the unload + load buffers for a platform, creating the load buffer if needed. */
	FBFPPlatform& SetupPlatform( AFGBuildableTrainPlatformCargo* Platform )
	{
		FBFPPlatform& P = GPlatforms.FindOrAdd( Platform );

		if ( !P.UnloadInventory.IsValid() )
		{
			UFGInventoryComponent* V = FindInventoryByName( Platform, TEXT( "inventory" ) );
			P.UnloadInventory = V ? V : Platform->GetInventory();
		}

		if ( !P.LoadInventory.IsValid() )
		{
			// The component owns the load buffer (creates it cloning the vanilla inventory, reuses the
			// saved one on reload). Centralising it here keeps a single creation path shared with the UI.
			if ( UBFPCargoPlatformComponent* C = EnsureToggleComponent( Platform ) )
			{
				P.LoadInventory = C->EnsureLoadInventory();
			}
		}

		return P;
	}

	/** One-line dump of the platform's docking-relevant state, with both buffer counts and bindings. */
	void LogPlatformState( const TCHAR* Event, AFGBuildableTrainPlatformCargo* Platform )
	{
		if ( !Platform )
		{
			return;
		}

		FBFPPlatform* P = GPlatforms.Find( Platform );
		int32 UnloadItems = -1;
		int32 LoadItems = -1;
		if ( P )
		{
			if ( P->UnloadInventory.IsValid() ) { UnloadItems = P->UnloadInventory->GetNumItems( nullptr ); }
			if ( P->LoadInventory.IsValid() )   { LoadItems = P->LoadInventory->GetNumItems( nullptr ); }
		}

		int32 WagonItems = -1;
		if ( AFGFreightWagon* Wagon = Cast<AFGFreightWagon>( Platform->GetDockedActor() ) )
		{
			if ( UFGInventoryComponent* WagonInv = Wagon->GetFreightInventory() )
			{
				WagonItems = WagonInv->GetNumItems( nullptr );
			}
		}

		UE_LOG( LogBFP, Verbose,
			TEXT( "[%s] %s | status=%s loadMode=%d activeInv=%p unloadBuf=%d loadBuf=%d wagon=%d docked=%s" ),
			Event, *Platform->GetName(), DockingStatusToString( Platform->GetDockingStatus() ),
			Platform->GetIsInLoadMode() ? 1 : 0, static_cast<const void*>( Platform->GetInventory() ),
			UnloadItems, LoadItems, WagonItems, *GetNameSafe( Platform->GetDockedActor() ) );	
	}
}

/**
 * Accumulate the docked wagon's freight delta during a stop (called every Factory_Tick while docked).
 * The game's own smoothed rate is unusable for us (it reads the inventory through mCargoInventoryHandler,
 * which our two-pass mInventory swaps confuse), so we derive it from the wagon's freight count:
 *   delta < 0 -> the wagon lost items -> UNLOAD; delta > 0 -> gained -> LOAD (attribution by sign).
 * The component rates are NOT touched here: the station cannot be opened while a train is docked, so we
 * publish the rate once at undock (FinalizeTransferRate) and keep it until the next stop.
 */
void SampleWagonTransferRate( AFGBuildableTrainPlatformCargo* Platform, FBFPPlatform& P )
{
	AFGFreightWagon* Wagon = Cast<AFGFreightWagon>( Platform->GetDockedActor() );
	UFGInventoryComponent* WInv = Wagon ? Wagon->GetFreightInventory() : nullptr;
	if ( !WInv )
	{
		return;
	}

	const int32 Count = WInv->GetNumItems( nullptr );
	if ( P.LastWagonCount < 0 )
	{
		P.LastWagonCount = Count; // baseline for this stop
		return;
	}

	const int32 Delta = Count - P.LastWagonCount;
	if ( Delta < 0 )      { P.UnloadMoved += -Delta; }
	else if ( Delta > 0 ) { P.LoadMoved   +=  Delta; }
	P.LastWagonCount = Count;
}

/**
 * Publish the just-finished stop's transfer rates (items/min) onto the component at undock, and keep
 * them there until the next stop. The player can only open the station once the train has left, so this
 * "last stop" value is the one they actually see. rate = items moved / configured (un)load time * 60.
 */
void FinalizeTransferRate( AFGBuildableTrainPlatformCargo* Platform, FBFPPlatform& P )
{
	if ( UBFPCargoPlatformComponent* C = P.ToggleComp.Get() )
	{
		const float TU = Platform->GetmTimeToCompleteUnload();
		const float TL = Platform->GetmTimeToCompleteLoad();
		const float UnloadRate = TU > 0.f ? P.UnloadMoved / TU * 60.f : 0.f;
		const float LoadRate   = TL > 0.f ? P.LoadMoved   / TL * 60.f : 0.f;
		// Sets the (replicated) rates AND fires OnTransferRateUpdated for the UI to refresh on.
		C->PublishTransferRates( LoadRate, UnloadRate );
		UE_LOG( LogBFP, Verbose,
			TEXT( "Rate FINALIZE %s: unloadMoved=%d loadMoved=%d -> unload=%.1f load=%.1f /min (event fired, held until next train)" ),
			*Platform->GetName(), P.UnloadMoved, P.LoadMoved, UnloadRate, LoadRate );
	}
}

void FBFPHooks::RegisterHooks()
{
	UE_LOG( LogBFP, Display, TEXT( "BidirectionalFreightPlatforms: registering hooks (transient-swap, save-safe)" ) );

// On spawn: resolve the two buffers and make sure mInventory rests on the unload buffer (this also
	// cleans up any save that was written by an older build while mInventory pointed at the load buffer).
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, BeginPlay,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			if ( !self || !IsSupportedPlatform(self ) )
			{
				return;
			}

			// Capture the vanilla inventory as the UNLOAD buffer (by name, robust against mInventory
			// pollution) and make mInventory rest on it. The LOAD buffer is created lazily later, once
			// the vanilla inventory is fully sized (so we can clone its capacity).
			UFGInventoryComponent* V = FindInventoryByName( self, TEXT( "inventory" ) );
			if ( !V )
			{
				V = self->GetInventory();
			}
			FBFPPlatform& P = GPlatforms.FindOrAdd( self );
			P.UnloadInventory = V;

			if ( V && self->GetInventory() != V )
			{
				UE_LOG( LogBFP, Warning, TEXT( "BeginPlay %s: mInventory was %p, restoring to unload buffer %p" ),
					*self->GetName(), static_cast<const void*>( self->GetInventory() ), static_cast<const void*>( V ) );
				self->SetmInventory( V );
			}

			// Create the toggle component and resolve its default mode now. BeginPlay is a safe context to
			// tell a loaded platform (world time ~0) from a newly-placed one (later), without a PostLoadGame
			// hook (which crashed dereferencing the object mid-deserialization).
			if ( UBFPCargoPlatformComponent* C = EnsureToggleComponent( self ) )
			{
				ResolveStationModeOnBeginPlay( self, C );
			}

			// Optional: point the platform's interact widget at our custom widget (replaces the station UI).
			// Solid and fluid use SEPARATE widget classes (item grids vs tanks/gauges), both configured from
			// Blueprint via UBFPBlueprintLibrary. Null for a type = keep that type's vanilla UI.
			const bool bFluid = self->GetmFreightCargoType() == EFreightCargoType::FCT_Liquid;
			const TSoftClassPtr<UFGInteractWidget> CustomWidget = bFluid
				? UBFPBlueprintLibrary::GetFluidStationInteractWidgetClass()
				: UBFPBlueprintLibrary::GetStationInteractWidgetClass();
			if ( !CustomWidget.IsNull() )
			{
				self->SetmInteractWidgetSoftClass( CustomWidget );
				UE_LOG( LogBFP, Verbose, TEXT( "Set custom interact widget on %s (%s)" ),
					*self->GetName(), bFluid ? TEXT( "fluid" ) : TEXT( "solid" ) );
			}

			UE_LOG( LogBFP, Verbose, TEXT( "BeginPlay %s | unloadBuf=%p" ),
				*self->GetName(), static_cast<const void*>( V ) );
		} );

	// Input redirection / blocking for solid platforms:
	//  - Load / Both: point mInventory at the load buffer for the collect so input belts feed it.
	//  - Unload only: the load side is OFF, so CANCEL the collect entirely — otherwise input belts would
	//    feed mInventory and pass straight through to the output belts.
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, Factory_CollectInput_Implementation,
		[]( auto& scope, AFGBuildableTrainPlatformCargo* self )
		{
			if ( self->GetmFreightCargoType() != EFreightCargoType::FCT_Standard )
			{
				return; // not a solid platform: let vanilla run
			}
			if ( ResolveStationMode( self ) == EBFPStationMode::Unload )
			{
				scope.Cancel(); // unload-only: input belts are inert (no pass-through)
				return;
			}
			FBFPPlatform& P = SetupPlatform( self );
			if ( P.LoadInventory.IsValid() )
			{
				if ( !P.bLoggedCollectFired )
				{
					P.bLoggedCollectFired = true;
					UE_LOG( LogBFP, Verbose, TEXT( "CollectInput hook ACTIVE on %s: inputs -> load buffer %p" ),
						*self->GetName(), static_cast<const void*>( P.LoadInventory.Get() ) );
				}
				P.CollectSaved = self->GetInventory();
				self->SetmInventory( P.LoadInventory.Get() );
			}
		} );
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, Factory_CollectInput_Implementation,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->CollectSaved.IsValid() )
				{
					self->SetmInventory( P->CollectSaved.Get() );
					P->CollectSaved = nullptr;
				}
			}
		} );

	// FLUID equivalent: liquid platforms pull input from pipes (Factory_PullPipeInput) instead of belts.
	//  - Load / Both: redirect the pull into the load buffer.
	//  - Unload only: cancel the pull so input pipes are inert (no fluid pass-through to the output).
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, Factory_PullPipeInput_Implementation,
		[]( auto& scope, AFGBuildableTrainPlatformCargo* self, float )
		{
			if ( self->GetmFreightCargoType() != EFreightCargoType::FCT_Liquid )
			{
				return;
			}
			if ( ResolveStationMode( self ) == EBFPStationMode::Unload )
			{
				scope.Cancel(); // unload-only: input pipes are inert (no pass-through)
				return;
			}
			FBFPPlatform& P = SetupPlatform( self );
			if ( P.LoadInventory.IsValid() )
			{
				if ( !P.bLoggedPipeFired )
				{
					P.bLoggedPipeFired = true;
					UE_LOG( LogBFP, Display, TEXT( "PullPipeInput hook ACTIVE on %s: pipes -> load buffer %p" ),
						*self->GetName(), static_cast<const void*>( P.LoadInventory.Get() ) );
				}
				P.PipeInputSaved = self->GetInventory();
				self->SetmInventory( P.LoadInventory.Get() );
			}
		} );
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, Factory_PullPipeInput_Implementation,
		[]( AFGBuildableTrainPlatformCargo* self, float )
		{
			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->PipeInputSaved.IsValid() )
				{
					self->SetmInventory( P->PipeInputSaved.Get() );
					P->PipeInputSaved = nullptr;
				}
			}
		} );

	// During the LOAD pass only, point mInventory at the load buffer for the Factory_Tick (which performs
	// the actual load transfer) and restore right after, so output/saves between ticks still see the unload buffer.
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, Factory_Tick,
		[]( auto&, AFGBuildableTrainPlatformCargo* self, float )
		{
			FBFPPlatform* P = GPlatforms.Find( self );
			if ( P && P->bLoadPass && P->LoadInventory.IsValid() )
			{
				P->TickSaved = self->GetInventory();
				self->SetmInventory( P->LoadInventory.Get() );
			}
		} );
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, Factory_Tick,
		[]( AFGBuildableTrainPlatformCargo* self, float )
		{
			FBFPPlatform* P = GPlatforms.Find( self );
			if ( !P )
			{
				return;
			}
			// Restore the load-pass bracket first, so the wagon-rate sample reads at-rest state.
			if ( P->TickSaved.IsValid() )
			{
				self->SetmInventory( P->TickSaved.Get() );
				P->TickSaved = nullptr;
			}
			SampleWagonTransferRate( self, *P );
		} );

	// A dock begins: set up the passes for this stop according to the station mode.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, NotifyTrainDocked,
		[]( AFGBuildableTrainPlatformCargo* self, AFGRailroadVehicle*, AFGBuildableRailroadStation* )
		{
			LogPlatformState( TEXT( "NotifyTrainDocked" ), self );
			if ( !IsSupportedPlatform(self ) )
			{
				return;
			}

			const EBFPStationMode Mode = ResolveStationMode( self );
			FBFPPlatform& P = SetupPlatform( self );
			P.bDockActive = true;
			P.bOriginalLoadMode = self->GetIsInLoadMode();

			// Fresh transfer-rate accounting for this stop; published to the component at undock.
			P.LastWagonCount = -1;
			P.UnloadMoved = 0;
			P.LoadMoved = 0;

			switch ( Mode )
			{
			case EBFPStationMode::Both:
				// Unload first (mInventory rests on the unload buffer), then a second LOAD pass is added
				// by the two-pass driver when the unload completes.
				P.bLoadPass = false;
				P.bTwoPass = true;
				self->SetmIsInLoadMode( false );
				UE_LOG( LogBFP, Verbose, TEXT( "Dock on %s: BOTH (unload-first)" ), *self->GetName() );
				break;

			case EBFPStationMode::Load:
				// Single load pass for the whole dock: mInventory is bracketed to the load buffer during
				// the transfer (same machinery as Both's load pass), no unload pass.
				P.bLoadPass = true;
				P.bTwoPass = false;
				self->SetmIsInLoadMode( true );
				UE_LOG( LogBFP, Verbose, TEXT( "Dock on %s: LOAD only" ), *self->GetName() );
				break;

			case EBFPStationMode::Unload:
			default:
				// Vanilla single unload pass: mInventory stays on the unload buffer, no brackets.
				P.bLoadPass = false;
				P.bTwoPass = false;
				self->SetmIsInLoadMode( false );
				UE_LOG( LogBFP, Verbose, TEXT( "Dock on %s: UNLOAD only" ), *self->GetName() );
				break;
			}
		} );

	// Bracket the dock-sequence evaluation during the load pass so its "can we load?" checks read the load buffer.
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, UpdateDockingSequence,
		[]( auto&, AFGBuildableTrainPlatformCargo* self )
		{
			FBFPPlatform* P = GPlatforms.Find( self );
			if ( P && P->bLoadPass && P->LoadInventory.IsValid() )
			{
				P->SeqSaved = self->GetInventory();
				self->SetmInventory( P->LoadInventory.Get() );
			}
		} );

	// Two-pass driver (runs after vanilla): when the unload pass completes, rewind to run a load pass.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, UpdateDockingSequence,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			LogPlatformState( TEXT( "UpdateDockingSequence" ), self );

			FBFPPlatform* P = GPlatforms.Find( self );
			if ( !P )
			{
				return;
			}

			const ETrainPlatformDockingStatus Status = self->GetDockingStatus();

			if ( P->bDockActive )
			{
				// Both mode only: unload pass finished -> start the LOAD pass (no persistent inventory swap;
				// the brackets do it). Load-only already has bLoadPass=true; Unload-only has bTwoPass=false.
				if ( P->bTwoPass && !P->bLoadPass && Status == ETrainPlatformDockingStatus::ETPDS_Complete )
				{
					P->bLoadPass = true;
					self->SetmIsInLoadMode( true );
					self->SetmPlatformDockingStatus( ETrainPlatformDockingStatus::ETPDS_WaitingToStart );
					UE_LOG( LogBFP, Verbose, TEXT( ">>> LOAD PASS on %s" ), *self->GetName() );
				}
				// Dock finished -> restore the configured mode, publish the stop's rates, clear per-dock state.
				else if ( Status == ETrainPlatformDockingStatus::ETPDS_None )
				{
					self->SetmIsInLoadMode( P->bOriginalLoadMode );
					P->bDockActive = false;
					P->bLoadPass = false;
					P->bTwoPass = false;
					// Train left: publish this stop's rates and keep them for the (now openable) station UI.
					FinalizeTransferRate( self, *P );
				}
			}

			// Undo the load-pass evaluation bracket so mInventory rests on the unload buffer.
			if ( P->SeqSaved.IsValid() )
			{
				self->SetmInventory( P->SeqSaved.Get() );
				P->SeqSaved = nullptr;
			}
		} );

	// Dock aborted mid-sequence: restore configured mode and clear state.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, CancelDockingSequence,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			LogPlatformState( TEXT( "CancelDockingSequence" ), self );
			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->bDockActive )
				{
					self->SetmIsInLoadMode( P->bOriginalLoadMode );
					P->bDockActive = false;
					P->bLoadPass = false;
					P->bTwoPass = false;
					FinalizeTransferRate( self, *P );
				}
				if ( P->UnloadInventory.IsValid() )
				{
					self->SetmInventory( P->UnloadInventory.Get() );
				}
			}
		} );
}

// Copyright Loupimo. All Rights Reserved.

#include "BFPHooks.h"

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Buildables/FGBuildableTrainPlatform.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "FGFreightWagon.h"            // AFGFreightWagon, EFreightCargoType (pulls in FGRailroadVehicle.h)
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"

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
		TWeakObjectPtr<UFGInventoryComponent> CollectSaved;
		TWeakObjectPtr<UFGInventoryComponent> TickSaved;
		TWeakObjectPtr<UFGInventoryComponent> SeqSaved;

		/** A dock is in progress and we are orchestrating its two passes. */
		bool bDockActive = false;
		/** The load (second) pass is running (mInventory should be the load buffer during bracketed calls). */
		bool bLoadPass = false;
		/** The platform's player-configured load mode, restored when the dock ends. */
		bool bOriginalLoadMode = false;

		/** Diagnostics: logged once when the input-redirect hook first fires. */
		bool bLoggedCollectFired = false;
	};

	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FBFPPlatform> GPlatforms;

	bool IsStandardPlatform( AFGBuildableTrainPlatformCargo* Platform )
	{
		return Platform && Platform->GetmFreightCargoType() == EFreightCargoType::FCT_Standard;
	}

	/** Bidirectional opt-in: a standard platform wired with BOTH an input and an output belt. */
	bool IsBidirectional( AFGBuildableTrainPlatformCargo* Platform )
	{
		if ( !IsStandardPlatform( Platform ) )
		{
			return false;
		}

		bool bHasInput = false;
		bool bHasOutput = false;
		for ( UFGFactoryConnectionComponent* Conn : Platform->GetConnectionComponents() )
		{
			if ( !Conn || !Conn->IsConnected() )
			{
				continue;
			}
			if ( Conn->GetDirection() == EFactoryConnectionDirection::FCD_INPUT )       { bHasInput = true; }
			else if ( Conn->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT ) { bHasOutput = true; }
		}
		return bHasInput && bHasOutput;
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
			// Reuse the saved-and-restored component on reload; only create one the first time.
			UFGInventoryComponent* L = FindInventoryByName( Platform, TEXT( "BFP_LoadInventory" ) );
			if ( !L )
			{
				L = NewObject<UFGInventoryComponent>( Platform, TEXT( "BFP_LoadInventory" ) );
				if ( L )
				{
					L->RegisterComponent();
					const int32 Size = FMath::Max( 1, static_cast<int32>( Platform->GetmStorageSizeX() ) * static_cast<int32>( Platform->GetmStorageSizeY() ) );
					L->Resize( Size );
					UE_LOG( LogBFP, Display, TEXT( "Created load buffer (%d slots) for %s" ), Size, *Platform->GetName() );
				}
			}
			P.LoadInventory = L;
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

		UE_LOG( LogBFP, Display,
			TEXT( "[%s] %s | status=%s loadMode=%d activeInv=%p unloadBuf=%d loadBuf=%d wagon=%d docked=%s" ),
			Event, *Platform->GetName(), DockingStatusToString( Platform->GetDockingStatus() ),
			Platform->GetIsInLoadMode() ? 1 : 0, static_cast<const void*>( Platform->GetInventory() ),
			UnloadItems, LoadItems, WagonItems, *GetNameSafe( Platform->GetDockedActor() ) );
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
			if ( !self || !IsStandardPlatform( self ) )
			{
				return;
			}

			FBFPPlatform& P = SetupPlatform( self );
			if ( P.UnloadInventory.IsValid() && self->GetInventory() != P.UnloadInventory.Get() )
			{
				UE_LOG( LogBFP, Warning, TEXT( "BeginPlay %s: mInventory was %p, restoring to unload buffer %p" ),
					*self->GetName(), static_cast<const void*>( self->GetInventory() ), static_cast<const void*>( P.UnloadInventory.Get() ) );
				self->SetmInventory( P.UnloadInventory.Get() );
			}

			UE_LOG( LogBFP, Display, TEXT( "BeginPlay %s | unloadBuf=%p loadBuf=%p bidirectional=%d" ),
				*self->GetName(), static_cast<const void*>( P.UnloadInventory.Get() ),
				static_cast<const void*>( P.LoadInventory.Get() ), IsBidirectional( self ) ? 1 : 0 );
		} );

	// Input redirection: point mInventory at the load buffer for the duration of the collect, restore after.
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, Factory_CollectInput_Implementation,
		[]( auto&, AFGBuildableTrainPlatformCargo* self )
		{
			if ( !IsBidirectional( self ) )
			{
				return;
			}
			FBFPPlatform& P = SetupPlatform( self );
			if ( P.LoadInventory.IsValid() )
			{
				if ( !P.bLoggedCollectFired )
				{
					P.bLoggedCollectFired = true;
					UE_LOG( LogBFP, Warning, TEXT( "CollectInput hook ACTIVE on %s: inputs -> load buffer %p" ),
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
			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->TickSaved.IsValid() )
				{
					self->SetmInventory( P->TickSaved.Get() );
					P->TickSaved = nullptr;
				}
			}
		} );

	// A dock begins: force the first pass to be UNLOAD (mInventory already rests on the unload buffer).
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, NotifyTrainDocked,
		[]( AFGBuildableTrainPlatformCargo* self, AFGRailroadVehicle*, AFGBuildableRailroadStation* )
		{
			LogPlatformState( TEXT( "NotifyTrainDocked" ), self );
			if ( !IsBidirectional( self ) )
			{
				return;
			}

			FBFPPlatform& P = SetupPlatform( self );
			P.bDockActive = true;
			P.bLoadPass = false;
			P.bOriginalLoadMode = self->GetIsInLoadMode();
			self->SetmIsInLoadMode( false );

			UE_LOG( LogBFP, Display, TEXT( "Bidirectional dock on %s: forcing UNLOAD-first" ), *self->GetName() );
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
				// Unload pass finished -> start the LOAD pass (no persistent inventory swap; the brackets do it).
				if ( Status == ETrainPlatformDockingStatus::ETPDS_Complete && !P->bLoadPass )
				{
					P->bLoadPass = true;
					self->SetmIsInLoadMode( true );
					self->SetmPlatformDockingStatus( ETrainPlatformDockingStatus::ETPDS_WaitingToStart );
					UE_LOG( LogBFP, Warning, TEXT( ">>> LOAD PASS on %s" ), *self->GetName() );
				}
				// Dock finished -> restore the configured mode and clear per-dock state.
				else if ( Status == ETrainPlatformDockingStatus::ETPDS_None )
				{
					self->SetmIsInLoadMode( P->bOriginalLoadMode );
					P->bDockActive = false;
					P->bLoadPass = false;
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
				}
				if ( P->UnloadInventory.IsValid() )
				{
					self->SetmInventory( P->UnloadInventory.Get() );
				}
			}
		} );
}

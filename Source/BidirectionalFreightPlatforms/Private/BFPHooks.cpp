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
	 * Per-platform state for the bidirectional behaviour. Lives in a transient map keyed by the
	 * platform (the load inventory is a registered component of the actor, so the actor keeps it
	 * alive; we only hold weak handles here). Persistence/replication is a later increment.
	 */
	struct FBFPPlatform
	{
		/** The platform's original/vanilla inventory: the UNLOAD buffer (wagon -> here -> output belts). */
		TWeakObjectPtr<UFGInventoryComponent> UnloadInventory;
		/** Our added inventory: the LOAD buffer (input belts -> here -> wagon). */
		TWeakObjectPtr<UFGInventoryComponent> LoadInventory;
		/** Saved mInventory pointer across a Factory_CollectInput swap (set in _BEFORE, restored in _AFTER). */
		TWeakObjectPtr<UFGInventoryComponent> CollectSaved;

		/** A dock is currently in progress and we are orchestrating its two passes. */
		bool bDockActive = false;
		/** The load (second) pass has been kicked off for the current dock. */
		bool bSecondPassStarted = false;
		/** The platform's player-configured load mode, restored when the dock ends. */
		bool bOriginalLoadMode = false;
	};

	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FBFPPlatform> GPlatforms;

	bool IsStandardPlatform( AFGBuildableTrainPlatformCargo* Platform )
	{
		return Platform && Platform->mFreightCargoType == EFreightCargoType::FCT_Standard;
	}

	/** Bidirectional opt-in: a standard platform wired with BOTH an input and an output belt. Single-
	 *  direction stations (one side wired) are left fully vanilla. Replaced by a real toggle later. */
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
			if ( Conn->GetDirection() == EFactoryConnectionDirection::FCD_INPUT )
			{
				bHasInput = true;
			}
			else if ( Conn->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT )
			{
				bHasOutput = true;
			}
		}
		return bHasInput && bHasOutput;
	}

	/** Lazily create the LOAD buffer inventory for a platform and remember the UNLOAD (vanilla) one. */
	UFGInventoryComponent* GetOrCreateLoadInventory( AFGBuildableTrainPlatformCargo* Platform )
	{
		FBFPPlatform& P = GPlatforms.FindOrAdd( Platform );

		if ( !P.UnloadInventory.IsValid() )
		{
			UFGInventoryComponent* CurrentInv = Platform->mInventory;
			P.UnloadInventory = CurrentInv;
		}

		if ( !P.LoadInventory.IsValid() )
		{
			UFGInventoryComponent* Inv = NewObject<UFGInventoryComponent>( Platform, TEXT( "BFP_LoadInventory" ) );
			if ( Inv )
			{
				Inv->RegisterComponent();
				const int32 Size = FMath::Max( 1, static_cast<int32>( Platform->mStorageSizeX ) * static_cast<int32>( Platform->mStorageSizeY ) );
				Inv->Resize( Size );
				P.LoadInventory = Inv;
				UE_LOG( LogBFP, Display, TEXT( "Created load buffer (%d slots) for %s" ), Size, *Platform->GetName() );
			}
		}

		return P.LoadInventory.Get();
	}

	/** One-line dump of the platform's docking-relevant state, with both buffer counts. */
	void LogPlatformState( const TCHAR* Event, AFGBuildableTrainPlatformCargo* Platform )
	{
		if ( !Platform )
		{
			return;
		}

		UFGInventoryComponent* PlatformInv = Platform->GetInventory();
		const int32 PlatformItems = PlatformInv ? PlatformInv->GetNumItems( nullptr ) : -1;

		int32 UnloadItems = -1;
		int32 LoadItems = -1;
		if ( FBFPPlatform* P = GPlatforms.Find( Platform ) )
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
			Event,
			*Platform->GetName(),
			DockingStatusToString( Platform->GetDockingStatus() ),
			Platform->GetIsInLoadMode() ? 1 : 0,
			static_cast<const void*>( PlatformInv ),
			UnloadItems,
			LoadItems,
			WagonItems,
			*GetNameSafe( Platform->GetDockedActor() ) );
	}
}

void FBFPHooks::RegisterHooks()
{
	UE_LOG( LogBFP, Display, TEXT( "BidirectionalFreightPlatforms: registering hooks (Increment 2: two inventories)" ) );

	// Dump the belt-connection layout when a platform spawns (kept for diagnostics).
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, BeginPlay,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			if ( !self )
			{
				return;
			}

			const TArray<UFGFactoryConnectionComponent*> Connections = self->GetConnectionComponents();
			UE_LOG( LogBFP, Display, TEXT( "BeginPlay %s | inv=%p | bidirectional=%d | %d connection(s):" ),
				*self->GetName(), static_cast<const void*>( self->GetInventory() ),
				IsBidirectional( self ) ? 1 : 0, Connections.Num() );

			for ( UFGFactoryConnectionComponent* Conn : Connections )
			{
				if ( !Conn )
				{
					continue;
				}
				UE_LOG( LogBFP, Display, TEXT( "    - %s dir=%d boundInv=%p connected=%d" ),
					*Conn->GetName(), static_cast<int32>( Conn->GetDirection() ),
					static_cast<const void*>( Conn->GetInventory() ), Conn->IsConnected() ? 1 : 0 );
			}
		} );

	// Input redirection: while the platform collects from its input belts, temporarily point mInventory
	// at the LOAD buffer so the vanilla collection fills it instead of the unload buffer. Restored right
	// after. Output grabbing is untouched (output connections stay bound to the unload-buffer object).
	SUBSCRIBE_UOBJECT_METHOD( AFGBuildableTrainPlatformCargo, Factory_CollectInput_Implementation,
		[]( auto& scope, AFGBuildableTrainPlatformCargo* self )
		{
			if ( !IsBidirectional( self ) )
			{
				return; // auto-forwards to vanilla
			}
			if ( UFGInventoryComponent* Load = GetOrCreateLoadInventory( self ) )
			{
				FBFPPlatform& P = GPlatforms.FindOrAdd( self );
				P.CollectSaved = self->mInventory;
				self->mInventory = Load;
			}
			// auto-forwards to vanilla, which now collects into the load buffer
		} );

	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, Factory_CollectInput_Implementation,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->CollectSaved.IsValid() )
				{
					self->mInventory = P->CollectSaved.Get();
					P->CollectSaved = nullptr;
				}
			}
		} );

	// A dock begins: for bidirectional platforms force the first pass to be UNLOAD (on the unload buffer)
	// regardless of the platform's configured mode, and set up our per-dock state.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, NotifyTrainDocked,
		[]( AFGBuildableTrainPlatformCargo* self, AFGRailroadVehicle* vehicle, AFGBuildableRailroadStation* station )
		{
			LogPlatformState( TEXT( "NotifyTrainDocked" ), self );

			if ( !IsBidirectional( self ) )
			{
				return;
			}

			GetOrCreateLoadInventory( self );
			FBFPPlatform& P = GPlatforms.FindOrAdd( self );
			P.bDockActive = true;
			P.bSecondPassStarted = false;
			P.bOriginalLoadMode = ( self->mIsInLoadMode != 0 );

			// Pass 1 = UNLOAD on the unload buffer.
			if ( P.UnloadInventory.IsValid() )
			{
				self->mInventory = P.UnloadInventory.Get();
			}
			self->mIsInLoadMode = 0;

			UE_LOG( LogBFP, Display, TEXT( "Bidirectional dock on %s: forcing UNLOAD-first" ), *self->GetName() );
		} );

	// Two-pass driver: when a pass completes, run the opposite direction on the opposite buffer.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, UpdateDockingSequence,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			LogPlatformState( TEXT( "UpdateDockingSequence" ), self );

			FBFPPlatform* P = GPlatforms.Find( self );
			if ( !P || !P->bDockActive )
			{
				return;
			}

			const ETrainPlatformDockingStatus Status = self->GetDockingStatus();

			// Unload pass finished -> start the LOAD pass on the load buffer.
			if ( Status == ETrainPlatformDockingStatus::ETPDS_Complete && !P->bSecondPassStarted )
			{
				P->bSecondPassStarted = true;
				self->mIsInLoadMode = 1;
				if ( P->LoadInventory.IsValid() )
				{
					self->mInventory = P->LoadInventory.Get();
				}
				self->mPlatformDockingStatus = ETrainPlatformDockingStatus::ETPDS_WaitingToStart;

				UE_LOG( LogBFP, Warning, TEXT( ">>> LOAD PASS on %s: mInventory -> load buffer (%p)" ),
					*self->GetName(), static_cast<const void*>( self->mInventory ) );
			}
			// Dock finished -> restore the platform to its default (unload buffer + configured mode).
			else if ( Status == ETrainPlatformDockingStatus::ETPDS_None )
			{
				if ( P->UnloadInventory.IsValid() )
				{
					self->mInventory = P->UnloadInventory.Get();
				}
				self->mIsInLoadMode = P->bOriginalLoadMode ? 1 : 0;
				P->bDockActive = false;
				P->bSecondPassStarted = false;
			}
		} );

	// Dock aborted mid-sequence: restore defaults.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGBuildableTrainPlatformCargo, CancelDockingSequence,
		[]( AFGBuildableTrainPlatformCargo* self )
		{
			LogPlatformState( TEXT( "CancelDockingSequence" ), self );

			if ( FBFPPlatform* P = GPlatforms.Find( self ) )
			{
				if ( P->bDockActive )
				{
					if ( P->UnloadInventory.IsValid() )
					{
						self->mInventory = P->UnloadInventory.Get();
					}
					self->mIsInLoadMode = P->bOriginalLoadMode ? 1 : 0;
					P->bDockActive = false;
					P->bSecondPassStarted = false;
				}
			}
		} );
}

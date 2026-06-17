// Copyright Loupimo. All Rights Reserved.

#include "BFPBlueprintLibrary.h"

#include "BFPHooks.h" // LogBFP category
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGInventoryComponent.h"
#include "BFPCargoPlatformComponent.h"
#include "FGHUD.h"
#include "UI/FGGameUI.h"
#include "GameFramework/PlayerController.h"
#include "FGPipeConnectionComponent.h"
#include "Buildables/FGBuildablePipeline.h"
#include "FGPipeSubsystem.h"
#include "BFPInteractProxyComponent.h"
#include "GameFramework/Pawn.h"

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::StationInteractWidgetClass = nullptr;
TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::FluidStationInteractWidgetClass = nullptr;

void UBFPBlueprintLibrary::SetStationInteractWidgetClass( TSoftClassPtr<UFGInteractWidget> WidgetClass )
{
	StationInteractWidgetClass = WidgetClass;
}

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::GetStationInteractWidgetClass()
{
	return StationInteractWidgetClass;
}

void UBFPBlueprintLibrary::SetFluidStationInteractWidgetClass( TSoftClassPtr<UFGInteractWidget> WidgetClass )
{
	FluidStationInteractWidgetClass = WidgetClass;
}

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::GetFluidStationInteractWidgetClass()
{
	return FluidStationInteractWidgetClass;
}

UFGInventoryComponent* UBFPBlueprintLibrary::GetUnloadInventory( AFGBuildableTrainPlatformCargo* Platform )
{
	// The vanilla inventory is our unload buffer; it always rests here between ticks.
	UFGInventoryComponent* Inv = Platform ? Platform->GetInventory() : nullptr;
	if ( Platform )
	{
		// DIAGNOSTIC (MP): is the vanilla inventory replicated to the client, and does its content reach us?
		UE_LOG( LogBFP, Verbose, TEXT( "[BFP] GetUnloadInventory on %s: hasAuthority=%d unloadBuf=%s items=%d size=%d" ),
			*Platform->GetName(), Platform->HasAuthority() ? 1 : 0,
			Inv ? TEXT( "FOUND" ) : TEXT( "NULL" ),
			Inv ? Inv->GetNumItems( nullptr ) : -1, Inv ? Inv->GetSizeLinear() : -1 );
	}
	return Inv;
}

UFGInventoryComponent* UBFPBlueprintLibrary::GetLoadInventory( AFGBuildableTrainPlatformCargo* Platform )
{
	if ( !Platform )
	{
		return nullptr;
	}

	UBFPCargoPlatformComponent* Comp = Platform->FindComponentByClass<UBFPCargoPlatformComponent>();
	UFGInventoryComponent* Inv = Comp ? Comp->GetLoadInventory() : nullptr;
	if ( !Inv && Comp )
	{
		// Server / single-player: lazily create so the UI always has something to show.
		// No-op (returns null) on clients, which receive it via replication instead.
		Inv = Comp->EnsureLoadInventory();
	}

	// DIAGNOSTIC (MP): when null, pinpoint whether the toggle COMPONENT failed to replicate to the client
	// (toggleComp=NULL) or only the load buffer did (toggleComp=FOUND). hasAuthority=0 means we are a client.
	UE_LOG( LogBFP, Verbose, TEXT( "[BFP] GetLoadInventory on %s: hasAuthority=%d toggleComp=%s loadBuf=%s" ),
		*Platform->GetName(), Platform->HasAuthority() ? 1 : 0,
		Comp ? TEXT( "FOUND" ) : TEXT( "NULL" ), Inv ? TEXT( "FOUND" ) : TEXT( "NULL" ) );
	return Inv;
}

float UBFPBlueprintLibrary::GetLoadItemTransferRate( AFGBuildableTrainPlatformCargo* Platform )
{
	// Our hooks compute this from the docked wagon's freight delta (the game's own smoothed rate is
	// unusable in our two-pass architecture). 0 between stops; reset on undock.
	if ( !Platform )
	{
		return 0.f;
	}
	UBFPCargoPlatformComponent* Comp = Platform->FindComponentByClass<UBFPCargoPlatformComponent>();
	return Comp ? Comp->GetLoadRate() : 0.f;
}

float UBFPBlueprintLibrary::GetUnloadItemTransferRate( AFGBuildableTrainPlatformCargo* Platform )
{
	if ( !Platform )
	{
		return 0.f;
	}
	UBFPCargoPlatformComponent* Comp = Platform->FindComponentByClass<UBFPCargoPlatformComponent>();
	return Comp ? Comp->GetUnloadRate() : 0.f;
}

void UBFPBlueprintLibrary::CloseStationUI( UFGInteractWidget* Widget )
{
	if ( !Widget )
	{
		return;
	}

	APlayerController* PC = Widget->GetOwningPlayer();
	AFGHUD* HUD = PC ? Cast<AFGHUD>( PC->GetHUD() ) : nullptr;
	if ( UFGGameUI* GameUI = HUD ? HUD->GetGameUI() : nullptr )
	{
		// Exactly what the Escape key triggers: pops the top interact widget and restores input/camera.
		GameUI->Native_HandlePauseGamePressed();
	}
}

namespace
{
	// SF stores fluid amounts as integers where 1 m³ = 1000 units. Divide to display m³.
	constexpr float BFP_FluidUnitsPerM3 = 1000.f;
}

float UBFPBlueprintLibrary::GetFluidAmountM3( UFGInventoryComponent* Inventory )
{
	return Inventory ? Inventory->GetNumItems( nullptr ) / BFP_FluidUnitsPerM3 : 0.f;
}

float UBFPBlueprintLibrary::GetFluidCapacityM3( UFGInventoryComponent* Inventory )
{
	if ( !Inventory || Inventory->GetSizeLinear() <= 0 )
	{
		return 0.f;
	}
	return Inventory->GetSlotSize( 0, nullptr ) / BFP_FluidUnitsPerM3;
}

TSubclassOf<UFGItemDescriptor> UBFPBlueprintLibrary::GetFluidClass( UFGInventoryComponent* Inventory )
{
	if ( !Inventory || Inventory->GetSizeLinear() <= 0 )
	{
		return nullptr;
	}
	TSubclassOf<UFGItemDescriptor> Cls = Inventory->GetItemClassAtIndex( 0 );
	if ( !Cls )
	{
		// Empty slot: fall back to the configured allowed fluid type, if any.
		Cls = Inventory->GetAllowedItemOnIndex( 0 );
	}
	return Cls;
}

float UBFPBlueprintLibrary::GetMaxPipeFlowRate( AFGBuildableTrainPlatformCargo* Platform )
{
	if ( !Platform )
	{
		return 0.f;
	}

	// Walk the platform's pipe connections to the pipelines actually attached, and take the best limit.
	float MaxLimit = 0.f;
	TInlineComponentArray<UFGPipeConnectionComponent*> PipeConns;
	Platform->GetComponents( PipeConns );
	for ( UFGPipeConnectionComponent* Conn : PipeConns )
	{
		if ( !Conn || !Conn->IsConnected() )
		{
			continue;
		}
		const UFGPipeConnectionComponentBase* Other = Conn->GetConnection();
		if ( const AFGBuildablePipeline* Pipe = Other ? Cast<AFGBuildablePipeline>( Other->GetOwner() ) : nullptr )
		{
			MaxLimit = FMath::Max( MaxLimit, Pipe->GetFlowLimit() );
		}
	}
	return MaxLimit;
}

void UBFPBlueprintLibrary::FlushStationPipes( AFGBuildableTrainPlatformCargo* Platform, bool bInput )
{
	if ( !Platform )
	{
		return;
	}
	AFGPipeSubsystem* Subsystem = AFGPipeSubsystem::GetPipeSubsystem( Platform );
	if ( !Subsystem )
	{
		return;
	}

	// Use the platform's own input/output connection arrays (authoritative in/out classification).
	const TArray<TObjectPtr<UFGPipeConnectionComponent>> Connections = bInput
		? Platform->GetmPipeInputConnections()
		: Platform->GetmPipeOutputConnections();
	for ( UFGPipeConnectionComponent* Conn : Connections )
	{
		if ( !Conn || !Conn->IsConnected() )
		{
			continue;
		}
		const UFGPipeConnectionComponentBase* Other = Conn->GetConnection();
		if ( AActor* Integrant = Other ? Other->GetOwner() : nullptr )
		{
			// Flushes the whole network the connected integrant belongs to (clears fluid + empties contents).
			Subsystem->FlushPipeNetworkFromIntegrant( Integrant );
		}
	}
}

UBFPInteractProxyComponent* UBFPBlueprintLibrary::GetInteractProxy( APawn* PlayerPawn )
{
	return PlayerPawn ? PlayerPawn->FindComponentByClass<UBFPInteractProxyComponent>() : nullptr;
}

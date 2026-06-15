// Copyright Loupimo. All Rights Reserved.

#include "BFPBlueprintLibrary.h"

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGInventoryComponent.h"
#include "BFPCargoPlatformComponent.h"
#include "FGHUD.h"
#include "UI/FGGameUI.h"
#include "GameFramework/PlayerController.h"

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::StationInteractWidgetClass = nullptr;

void UBFPBlueprintLibrary::SetStationInteractWidgetClass( TSoftClassPtr<UFGInteractWidget> WidgetClass )
{
	StationInteractWidgetClass = WidgetClass;
}

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::GetStationInteractWidgetClass()
{
	return StationInteractWidgetClass;
}

UFGInventoryComponent* UBFPBlueprintLibrary::GetUnloadInventory( AFGBuildableTrainPlatformCargo* Platform )
{
	// The vanilla inventory is our unload buffer; it always rests here between ticks.
	return Platform ? Platform->GetInventory() : nullptr;
}

UFGInventoryComponent* UBFPBlueprintLibrary::GetLoadInventory( AFGBuildableTrainPlatformCargo* Platform )
{
	if ( !Platform )
	{
		return nullptr;
	}

	UBFPCargoPlatformComponent* Comp = Platform->FindComponentByClass<UBFPCargoPlatformComponent>();
	if ( !Comp )
	{
		return nullptr;
	}

	UFGInventoryComponent* Inv = Comp->GetLoadInventory();
	if ( !Inv )
	{
		// Server / single-player: lazily create so the UI always has something to show.
		// No-op (returns null) on clients, which receive it via replication instead.
		Inv = Comp->EnsureLoadInventory();
	}
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

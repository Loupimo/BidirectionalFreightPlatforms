// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UI/FGInteractWidget.h"
#include "BFPBlueprintLibrary.generated.h"

class AFGBuildableTrainPlatformCargo;
class UFGInventoryComponent;

/**
 * Small library to feed mod-wide settings from Blueprint (e.g. your GameInstance / GameWorld module)
 * to the native hooks, so we never hardcode content paths in C++.
 */
UCLASS()
class BIDIRECTIONALFREIGHTPLATFORMS_API UBFPBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Set the widget class that replaces the freight platform station UI.
	 * Call this once at startup from your module (expose an EditDefaultsOnly TSoftClassPtr<UFGInteractWidget>
	 * on the module and pass it here). Leave unset to keep the vanilla UI.
	 */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	static void SetStationInteractWidgetClass( TSoftClassPtr<UFGInteractWidget> WidgetClass );

	/** The configured station UI widget class (null = keep vanilla). Read by the native hooks. */
	static TSoftClassPtr<UFGInteractWidget> GetStationInteractWidgetClass();

	/**
	 * UNLOAD buffer (wagon -> output belts) = the vanilla inventory. This is the "outgoing" panel.
	 * Always rests on the unload buffer between ticks, so it is safe to read from the UI.
	 */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	static UFGInventoryComponent* GetUnloadInventory( AFGBuildableTrainPlatformCargo* Platform );

	/**
	 * LOAD buffer (input belts -> wagon) = our modded inventory. This is the "incoming" panel.
	 * Resolved from the BFP component; created on demand on the server / single-player.
	 */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	static UFGInventoryComponent* GetLoadInventory( AFGBuildableTrainPlatformCargo* Platform );

	/**
	 * Smoothed LOAD transfer rate (items/min, input belts -> wagon). Maintained & replicated by the
	 * game; correctly attributed because our two-pass dock sets mIsInLoadMode during the load pass.
	 * Poll this from the widget (e.g. OnCustomTick / a ~0.5-1s timer).
	 */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	static float GetLoadItemTransferRate( AFGBuildableTrainPlatformCargo* Platform );

	/** Smoothed UNLOAD transfer rate (items/min, wagon -> output belts). See GetLoadItemTransferRate. */
	UFUNCTION( BlueprintPure, Category = "BidirectionalFreightPlatforms" )
	static float GetUnloadItemTransferRate( AFGBuildableTrainPlatformCargo* Platform );

	/**
	 * Close this station UI exactly like the Escape key does: pops the interact widget off the
	 * GameUI stack and restores game input + camera. Call from your close (X) button.
	 * Works on the top of the stack, so it does not matter which sub-widget calls it.
	 */
	UFUNCTION( BlueprintCallable, Category = "BidirectionalFreightPlatforms" )
	static void CloseStationUI( UFGInteractWidget* Widget );

private:
	static TSoftClassPtr<UFGInteractWidget> StationInteractWidgetClass;
};

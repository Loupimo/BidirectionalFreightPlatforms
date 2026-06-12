// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UI/FGInteractWidget.h"
#include "BFPBlueprintLibrary.generated.h"

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

private:
	static TSoftClassPtr<UFGInteractWidget> StationInteractWidgetClass;
};

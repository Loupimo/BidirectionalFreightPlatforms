// Copyright Loupimo. All Rights Reserved.

#include "BFPBlueprintLibrary.h"

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::StationInteractWidgetClass = nullptr;

void UBFPBlueprintLibrary::SetStationInteractWidgetClass( TSoftClassPtr<UFGInteractWidget> WidgetClass )
{
	StationInteractWidgetClass = WidgetClass;
}

TSoftClassPtr<UFGInteractWidget> UBFPBlueprintLibrary::GetStationInteractWidgetClass()
{
	return StationInteractWidgetClass;
}

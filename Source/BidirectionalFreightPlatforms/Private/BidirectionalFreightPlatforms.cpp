// Copyright Epic Games, Inc. All Rights Reserved.

#include "BidirectionalFreightPlatforms.h"

#include "BFPHooks.h"

#define LOCTEXT_NAMESPACE "FBidirectionalFreightPlatformsModule"

void FBidirectionalFreightPlatformsModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

	// Install the freight-platform hooks.
	if (!WITH_EDITOR)
	{
		FBFPHooks::RegisterHooks();
	}
}

void FBidirectionalFreightPlatformsModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FBidirectionalFreightPlatformsModule, BidirectionalFreightPlatforms)
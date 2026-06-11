// Copyright Loupimo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN( LogBFP, Log, All );

/**
 * Phase 0 instrumentation.
 *
 * Installs read-only logging hooks on freight cargo platforms so we can reverse-engineer two things
 * that are defined in compiled game code / Blueprints and therefore not visible in source:
 *   1. The belt-connection layout (do platforms have permanent separate INPUT vs OUTPUT connectors,
 *      or do the connectors flip direction with the load/unload mode?).
 *   3. The docking state-machine flow (the order of ETrainPlatformDockingStatus transitions and the
 *      calls into NotifyTrainDocked / UpdateDockingSequence / CancelDockingSequence during a real stop).
 *
 * No gameplay is changed here — this build only writes to the LogBFP category. Once the flow is
 * understood the real bidirectional mechanism (two inventories + two-pass dock) is layered on top.
 */
class FBFPHooks
{
public:
	/** Called once from the module StartupModule() (game only, not in the editor cooker). */
	static void RegisterHooks();
};

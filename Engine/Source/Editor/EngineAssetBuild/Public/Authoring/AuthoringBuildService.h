#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/Texture2DAuthoringCoordinator.h"

namespace Durin::AssetBuild
{
	// Process-wide, read-only authoring workload snapshot for hosts and diagnostics.
	struct FAuthoringBuildServiceSnapshot
	{
		uint32 QueuedRequestCount = 0;
		uint32 RunningRequestCount = 0;
		uint64 InFlightEstimatedBytes = 0;
		bool bAcceptingRequests = false;
	};

	struct FAuthoringBuildServiceConfig
	{
		FTexture2DBuildCoordinatorConfig Texture2D;
	};

	// Starts after the process task system. Asset-family request types stay narrow;
	// this service owns their common process lifetime, frame pump, waits and drain.
	ENGINEASSETBUILD_API auto InitializeAuthoringBuildService(
		const FAuthoringBuildServiceConfig& Config = {}) -> bool;
	ENGINEASSETBUILD_API auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*;
	ENGINEASSETBUILD_API auto GetAuthoringBuildServiceSnapshot()
		-> FAuthoringBuildServiceSnapshot;
	ENGINEASSETBUILD_API auto PumpAuthoringBuildCompletions(
		uint32 MaximumCount = 64) -> uint32;
	ENGINEASSETBUILD_API auto WaitForAuthoringBuildService(
		double TimeoutSeconds = 30.0) -> bool;
	// Stops admission, cancels work, joins workers and drains completion callbacks.
	ENGINEASSETBUILD_API auto ShutdownAuthoringBuildService() -> void;
}

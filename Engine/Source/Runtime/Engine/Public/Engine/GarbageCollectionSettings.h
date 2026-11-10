#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"

namespace Durin
{
	// Configures time, garbage-pressure, and object-growth triggers for automatic collection.
	struct FGarbageCollectionSettings
	{
		bool bEnabled = true;
		double IntervalSeconds = 60.0;
		double MaxIntervalSeconds = 600.0;
		double IntervalBackoffMultiplier = 2.0;
		uint64 PendingKillThreshold = 128;
		uint64 ObjectGrowthThreshold = 1024;
	};

	ENGINE_API auto ConfigureAutomaticGarbageCollection(const FGarbageCollectionSettings& Settings, double CurrentTime) -> void;
}

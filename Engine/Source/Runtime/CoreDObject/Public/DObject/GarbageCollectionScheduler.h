#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	struct FGarbageCollectionSettings
	{
		bool bEnabled = true;
		double IntervalSeconds = 60.0;
		uint64 PendingKillThreshold = 128;
		uint64 ObjectGrowthThreshold = 1024;
	};

	enum class EGarbageCollectionTrigger : uint8
	{
		None,
		Interval,
		PendingKillPressure,
		ObjectGrowthPressure,
	};

	class FGarbageCollectionScheduler
	{
	public:
		COREDOBJECT_API explicit FGarbageCollectionScheduler(FGarbageCollectionSettings InSettings = {});
		COREDOBJECT_API auto Reset(double CurrentTime, uint64 CurrentObjectCount) -> void;
		COREDOBJECT_API auto ShouldCollect(double CurrentTime, uint64 CurrentObjectCount, uint64 PendingKillCount) const -> EGarbageCollectionTrigger;
		auto GetSettings() const -> const FGarbageCollectionSettings& { return Settings; }

	private:
		FGarbageCollectionSettings Settings;
		double LastCollectionTime = 0.0;
		uint64 ObjectCountAfterLastCollection = 0;
	};

	COREDOBJECT_API auto ConfigureAutomaticGarbageCollection(const FGarbageCollectionSettings& Settings, double CurrentTime) -> void;
	COREDOBJECT_API auto NotifyGarbageCollectionCompleted(double CurrentTime) -> void;
	COREDOBJECT_API auto TryCollectGarbage(double CurrentTime) -> EGarbageCollectionTrigger;
}

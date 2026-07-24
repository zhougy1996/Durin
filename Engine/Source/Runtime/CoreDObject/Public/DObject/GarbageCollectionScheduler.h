#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	struct FGarbageCollectionStats;

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

	// Identifies the policy condition that requested a garbage collection.
	enum class EGarbageCollectionTrigger : uint8
	{
		None,
		Interval,
		PendingKillPressure,
		ObjectGrowthPressure,
	};

	// Applies adaptive interval and pressure thresholds without owning the collector itself.
	class FGarbageCollectionScheduler
	{
	public:
		COREDOBJECT_API explicit FGarbageCollectionScheduler(FGarbageCollectionSettings InSettings = {});
		COREDOBJECT_API auto Reset(double CurrentTime, uint64 CurrentObjectCount) -> void;
		COREDOBJECT_API auto NotifyCollectionCompleted(double CurrentTime, uint64 CurrentObjectCount, uint64 CandidateObjectCount) -> void;
		COREDOBJECT_API auto ShouldCollect(double CurrentTime, uint64 CurrentObjectCount, uint64 PendingKillCount) const -> EGarbageCollectionTrigger;
		auto GetSettings() const -> const FGarbageCollectionSettings& { return Settings; }
		auto GetCurrentIntervalSeconds() const -> double { return CurrentIntervalSeconds; }

	private:
		FGarbageCollectionSettings Settings;
		double CurrentIntervalSeconds = 0.0;
		double LastCollectionTime = 0.0;
		uint64 ObjectCountAfterLastCollection = 0;
	};

	COREDOBJECT_API auto ConfigureAutomaticGarbageCollection(const FGarbageCollectionSettings& Settings, double CurrentTime) -> void;
	COREDOBJECT_API auto NotifyGarbageCollectionCompleted(double CurrentTime, const FGarbageCollectionStats& Stats) -> void;
	COREDOBJECT_API auto GetCurrentAutomaticGarbageCollectionIntervalSeconds() -> double;
	COREDOBJECT_API auto TryCollectGarbage(double CurrentTime) -> EGarbageCollectionTrigger;
}

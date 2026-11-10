#pragma once

#include "CoreMinimal.h"
#include "Engine/GarbageCollectionSettings.h"

namespace Durin { struct FGarbageCollectionStats; }

namespace Durin::Private
{
	// Identifies the policy condition that requested a garbage collection.
	enum class EGarbageCollectionTrigger : uint8
	{
		None,
		Requested,
		Interval,
		PendingKillPressure,
		ObjectGrowthPressure,
	};

	// Applies adaptive interval and pressure thresholds without owning the collector itself.
	class FGarbageCollectionScheduler
	{
	public:
		ENGINE_API explicit FGarbageCollectionScheduler(FGarbageCollectionSettings InSettings = {});
		ENGINE_API auto Reset(double CurrentTime, uint64 CurrentObjectCount) -> void;
		ENGINE_API auto NotifyCollectionCompleted(double CurrentTime, uint64 CurrentObjectCount, uint64 CandidateObjectCount) -> void;
		ENGINE_API auto ShouldCollect(double CurrentTime, uint64 CurrentObjectCount, uint64 PendingKillCount) const -> EGarbageCollectionTrigger;
		auto GetSettings() const -> const FGarbageCollectionSettings& { return Settings; }
		auto GetCurrentIntervalSeconds() const -> double { return CurrentIntervalSeconds; }

	private:
		FGarbageCollectionSettings Settings;
		double CurrentIntervalSeconds = 0.0;
		double LastCollectionTime = 0.0;
		uint64 ObjectCountAfterLastCollection = 0;
	};

	// Engine-owned policy; Core has no scheduling dependency.
	ENGINE_API auto EvaluateGarbageCollection(double CurrentTime) -> EGarbageCollectionTrigger;
	ENGINE_API auto GetCurrentAutomaticGarbageCollectionIntervalSeconds() -> double;
	auto ConditionalCollectGarbage(std::optional<uint64>& LastCheckedFrame) -> void;
}

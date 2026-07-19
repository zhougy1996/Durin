#include "DObject/GarbageCollectionScheduler.h"

#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin
{
	namespace
	{
		FGarbageCollectionScheduler GGarbageCollectionScheduler;
	}

	FGarbageCollectionScheduler::FGarbageCollectionScheduler(FGarbageCollectionSettings InSettings)
		: Settings(InSettings)
	{
		if (Settings.IntervalSeconds > 0.0 && Settings.MaxIntervalSeconds < Settings.IntervalSeconds)
		{
			Settings.MaxIntervalSeconds = Settings.IntervalSeconds;
		}
		if (Settings.IntervalBackoffMultiplier < 1.0) Settings.IntervalBackoffMultiplier = 1.0;
		CurrentIntervalSeconds = Settings.IntervalSeconds;
	}

	auto FGarbageCollectionScheduler::Reset(double CurrentTime, uint64 CurrentObjectCount) -> void
	{
		LastCollectionTime = CurrentTime;
		ObjectCountAfterLastCollection = CurrentObjectCount;
		CurrentIntervalSeconds = Settings.IntervalSeconds;
	}

	auto FGarbageCollectionScheduler::NotifyCollectionCompleted(double CurrentTime, uint64 CurrentObjectCount, uint64 CandidateObjectCount) -> void
	{
		LastCollectionTime = CurrentTime;
		ObjectCountAfterLastCollection = CurrentObjectCount;
		if (Settings.IntervalSeconds <= 0.0) return;

		if (CandidateObjectCount > 0)
		{
			CurrentIntervalSeconds = Settings.IntervalSeconds;
			return;
		}

		// A bounded interval remains the correctness fallback for unreachable objects
		// created solely by reference removal, which currently has no write barrier.
		CurrentIntervalSeconds = std::min(
			Settings.MaxIntervalSeconds,
			CurrentIntervalSeconds * Settings.IntervalBackoffMultiplier
		);
	}

	auto FGarbageCollectionScheduler::ShouldCollect(double CurrentTime, uint64 CurrentObjectCount, uint64 PendingKillCount) const -> EGarbageCollectionTrigger
	{
		if (!Settings.bEnabled) return EGarbageCollectionTrigger::None;
		if (Settings.PendingKillThreshold > 0 && PendingKillCount >= Settings.PendingKillThreshold) return EGarbageCollectionTrigger::PendingKillPressure;
		if (Settings.ObjectGrowthThreshold > 0 && CurrentObjectCount >= ObjectCountAfterLastCollection + Settings.ObjectGrowthThreshold) return EGarbageCollectionTrigger::ObjectGrowthPressure;
		if (CurrentIntervalSeconds > 0.0 && CurrentTime - LastCollectionTime >= CurrentIntervalSeconds) return EGarbageCollectionTrigger::Interval;
		return EGarbageCollectionTrigger::None;
	}

	auto ConfigureAutomaticGarbageCollection(const FGarbageCollectionSettings& Settings, double CurrentTime) -> void
	{
		GGarbageCollectionScheduler = FGarbageCollectionScheduler(Settings);
		GGarbageCollectionScheduler.Reset(CurrentTime, GDObjectArray.GetNum());
	}

	auto TryCollectGarbage(double CurrentTime) -> EGarbageCollectionTrigger
	{
		const EGarbageCollectionTrigger Trigger = GGarbageCollectionScheduler.ShouldCollect(CurrentTime, GDObjectArray.GetNum(), GetGarbageObjectCount());
		if (Trigger != EGarbageCollectionTrigger::None)
		{
			CollectGarbage();
		}
		return Trigger;
	}

	auto NotifyGarbageCollectionCompleted(double CurrentTime, const FGarbageCollectionStats& Stats) -> void
	{
		GGarbageCollectionScheduler.NotifyCollectionCompleted(CurrentTime, GDObjectArray.GetNum(), Stats.CandidateObjectCount);
	}

	auto GetCurrentAutomaticGarbageCollectionIntervalSeconds() -> double
	{
		return GGarbageCollectionScheduler.GetCurrentIntervalSeconds();
	}
}

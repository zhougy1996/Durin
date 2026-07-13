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
	}

	auto FGarbageCollectionScheduler::Reset(double CurrentTime, uint64 CurrentObjectCount) -> void
	{
		LastCollectionTime = CurrentTime;
		ObjectCountAfterLastCollection = CurrentObjectCount;
	}

	auto FGarbageCollectionScheduler::ShouldCollect(double CurrentTime, uint64 CurrentObjectCount, uint64 PendingKillCount) const -> EGarbageCollectionTrigger
	{
		if (!Settings.bEnabled) return EGarbageCollectionTrigger::None;
		if (Settings.PendingKillThreshold > 0 && PendingKillCount >= Settings.PendingKillThreshold) return EGarbageCollectionTrigger::PendingKillPressure;
		if (Settings.ObjectGrowthThreshold > 0 && CurrentObjectCount >= ObjectCountAfterLastCollection + Settings.ObjectGrowthThreshold) return EGarbageCollectionTrigger::ObjectGrowthPressure;
		if (Settings.IntervalSeconds > 0.0 && CurrentTime - LastCollectionTime >= Settings.IntervalSeconds) return EGarbageCollectionTrigger::Interval;
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

	auto NotifyGarbageCollectionCompleted(double CurrentTime) -> void
	{
		GGarbageCollectionScheduler.Reset(CurrentTime, GDObjectArray.GetNum());
	}
}

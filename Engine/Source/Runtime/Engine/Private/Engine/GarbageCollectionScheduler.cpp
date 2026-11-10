#include "GarbageCollectionScheduler.h"
#include "DObject/Internal/GarbageCollection.h"
#include "Misc/Time.h"
#include "Misc/AppConfig.h"
#include "Logging/LogMacros.h"

#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Private
{
	namespace
	{
		FGarbageCollectionScheduler GGarbageCollectionScheduler;

		uint64 GObservedCollectionSerial = 0;
		bool GConfigured = false;

		auto EnsureConfigured() -> void
		{
			if (GConfigured) return;
			// Preserve the existing config key while Engine owns its interpretation.
			const FYamlNodeView GCConfig = GetModuleConfig("CoreDObject").GetView("GC");
			FGarbageCollectionSettings GCSettings;
			GCSettings.bEnabled = GCConfig.GetView("Enabled").GetBool(true);
			GCSettings.IntervalSeconds = GCConfig.GetView("IntervalSeconds").GetDouble(60.0);
			GCSettings.MaxIntervalSeconds = GCConfig.GetView("MaxIntervalSeconds").GetDouble(600.0);
			GCSettings.IntervalBackoffMultiplier = GCConfig.GetView("IntervalBackoffMultiplier").GetDouble(2.0);
			GCSettings.PendingKillThreshold = GCConfig.GetView("PendingKillThreshold").GetUInt(128);
			GCSettings.ObjectGrowthThreshold = GCConfig.GetView("ObjectGrowthThreshold").GetUInt(1024);
			ConfigureAutomaticGarbageCollection(GCSettings, FTime::Seconds());
		}

		auto ObserveCollection() -> void
		{
			EnsureConfigured();
			const auto& Stats = GetLastGarbageCollectionStats();
			if (Stats.CollectionSerial == GObservedCollectionSerial) return;
			GObservedCollectionSerial = Stats.CollectionSerial;
			GGarbageCollectionScheduler.NotifyCollectionCompleted(Stats.CollectionTime,
				Stats.ObjectCountAfterCollection, Stats.CandidateObjectCount);
		}
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
		// created solely by reference removal. Synchronous tracing needs no write barrier.
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

	auto EvaluateGarbageCollection(double CurrentTime) -> EGarbageCollectionTrigger
	{
		ObserveCollection();
		return IsGarbageCollectionRequested() ? EGarbageCollectionTrigger::Requested
			: GGarbageCollectionScheduler.ShouldCollect(CurrentTime, GDObjectArray.GetNum(), GetGarbageObjectCount());
	}

	auto GetCurrentAutomaticGarbageCollectionIntervalSeconds() -> double
	{
		ObserveCollection();
		return GGarbageCollectionScheduler.GetCurrentIntervalSeconds();
	}

	auto ConditionalCollectGarbage(std::optional<uint64>& LastCheckedFrame) -> void
	{
		if (IsGarbageCollecting() || LastCheckedFrame == GFrameCounter) return;
		LastCheckedFrame = GFrameCounter; // A check counts even when policy does not collect.
		ObserveCollection();
		if (IsIncrementalPurgePending() && !IsFullPurgeRequested())
		{
			IncrementalPurgeGarbage();
			return; // Pending requests survive destruction-only advancement.
		}
		const auto& Stats = GetLastGarbageCollectionStats();
		if (Stats.CollectionSerial && Stats.CollectionFrame == GFrameCounter) return;
		const auto Trigger = EvaluateGarbageCollection(FTime::Seconds());
		if (Trigger == EGarbageCollectionTrigger::None) return;
		if (TryCollectGarbage(GIsEditor ? EObjectFlags::Standalone : EObjectFlags::NoFlags,
			IsFullPurgeRequested()))
		{
			const auto Name = [Trigger]() -> std::string_view {
				switch (Trigger)
				{
				case EGarbageCollectionTrigger::Requested: return "request";
				case EGarbageCollectionTrigger::Interval: return "interval";
				case EGarbageCollectionTrigger::PendingKillPressure: return "garbage pressure";
				case EGarbageCollectionTrigger::ObjectGrowthPressure: return "object growth";
				default: return "none";
				}
			}();
			DURIN_INFO_CATEGORY("GC", "Trigger: {}", Name);
		}
	}
}

namespace Durin
{
	auto ConfigureAutomaticGarbageCollection(const FGarbageCollectionSettings& Settings, double CurrentTime) -> void
	{
		Private::GGarbageCollectionScheduler = Private::FGarbageCollectionScheduler(Settings);
		Private::GGarbageCollectionScheduler.Reset(CurrentTime, GDObjectArray.GetNum());
		Private::GObservedCollectionSerial = GetLastGarbageCollectionStats().CollectionSerial;
		Private::GConfigured = true;
	}

}

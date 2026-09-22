#include "Engine/EngineGlobals.h"

#include "Misc/Time.h"

#include <algorithm>

namespace Durin
{
	namespace
	{
		FEngineFrameTiming GEngineFrameTiming;
		FEngineFrameTiming GCurrentFrameTiming;
		bool GFrameTimingInitialized = false;

		auto SmoothFrameMetric(float Current, float Sample) -> float
		{
			constexpr float Blend = 0.25f;
			return Current * (1.0f - Blend) + Sample * Blend;
		}
	}

	float GAverageFPS = 0.0f;
	float GAverageMS = 0.0f;

	auto RecordEngineFrameTickTiming(float EngineTickMilliseconds) -> void
	{
		GCurrentFrameTiming.EngineTickMilliseconds = std::max(EngineTickMilliseconds, 0.0f);
	}

	auto RecordEngineFrameRenderTimings(float UIFrameBuildMilliseconds,
		float SceneSubmissionMilliseconds, float UISubmissionMilliseconds, float SyncWaitMilliseconds) -> void
	{
		GCurrentFrameTiming.UIFrameBuildMilliseconds = std::max(UIFrameBuildMilliseconds, 0.0f);
		GCurrentFrameTiming.SceneSubmissionMilliseconds = std::max(SceneSubmissionMilliseconds, 0.0f);
		GCurrentFrameTiming.UISubmissionMilliseconds = std::max(UISubmissionMilliseconds, 0.0f);
		GCurrentFrameTiming.RenderSyncWaitMilliseconds = std::max(SyncWaitMilliseconds, 0.0f);
	}

	auto CalculateFPSTimings() -> void
	{
		static double LastTime = 0.0;
		const double CurrentTime = FTime::Seconds();
		if (LastTime <= 0.0)
		{
			LastTime = CurrentTime;
			GCurrentFrameTiming = {};
			return;
		}
		const float FrameTimeMS = static_cast<float>(
			(CurrentTime - LastTime) * 1000.0);
		LastTime = CurrentTime;
		GCurrentFrameTiming.FrameIntervalMilliseconds = FrameTimeMS;
		const FEngineFrameTiming Sample = GCurrentFrameTiming;
		GCurrentFrameTiming = {};

		if (!GFrameTimingInitialized)
		{
			GEngineFrameTiming = Sample;
			GFrameTimingInitialized = true;
		}
		else
		{
			GEngineFrameTiming.FrameIntervalMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.FrameIntervalMilliseconds, FrameTimeMS);
			GEngineFrameTiming.EngineTickMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.EngineTickMilliseconds, Sample.EngineTickMilliseconds);
			GEngineFrameTiming.UIFrameBuildMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.UIFrameBuildMilliseconds, Sample.UIFrameBuildMilliseconds);
			GEngineFrameTiming.SceneSubmissionMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.SceneSubmissionMilliseconds, Sample.SceneSubmissionMilliseconds);
			GEngineFrameTiming.UISubmissionMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.UISubmissionMilliseconds, Sample.UISubmissionMilliseconds);
			GEngineFrameTiming.RenderSyncWaitMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.RenderSyncWaitMilliseconds, Sample.RenderSyncWaitMilliseconds);
		}
		GAverageMS = GEngineFrameTiming.FrameIntervalMilliseconds;
		GAverageFPS = GAverageMS > 0.0f ? 1000.0f / GAverageMS : 0.0f;
	}

	auto GetEngineFrameTiming() -> const FEngineFrameTiming&
	{
		return GEngineFrameTiming;
	}
}

#include "EngineGlobals.h"

#include "Misc/Time.h"

#include <algorithm>

namespace Durin
{
	namespace
	{
		FEngineFrameTiming GEngineFrameTiming;
		float GCurrentRenderSyncWaitMilliseconds = 0.0f;
		bool GFrameTimingInitialized = false;

		auto SmoothFrameMetric(float Current, float Sample) -> float
		{
			constexpr float Blend = 0.25f;
			return Current * (1.0f - Blend) + Sample * Blend;
		}
	}

	float GAverageFPS = 0.0f;
	float GAverageMS = 0.0f;

	auto RecordEngineFrameSyncWait(float Milliseconds) -> void
	{
		GCurrentRenderSyncWaitMilliseconds = std::max(Milliseconds, 0.0f);
	}

	auto CalculateFPSTimings() -> void
	{
		static double LastTime = 0.0;
		const double CurrentTime = FTime::Seconds();
		if (LastTime <= 0.0)
		{
			LastTime = CurrentTime;
			GCurrentRenderSyncWaitMilliseconds = 0.0f;
			return;
		}
		const float FrameTimeMS = static_cast<float>(
			(CurrentTime - LastTime) * 1000.0);
		LastTime = CurrentTime;
		const float RenderSyncWaitMS = std::clamp(
			GCurrentRenderSyncWaitMilliseconds, 0.0f, FrameTimeMS);
		const float GameThreadWorkMS = FrameTimeMS - RenderSyncWaitMS;
		GCurrentRenderSyncWaitMilliseconds = 0.0f;

		if (!GFrameTimingInitialized)
		{
			GEngineFrameTiming = {FrameTimeMS, GameThreadWorkMS, RenderSyncWaitMS};
			GFrameTimingInitialized = true;
		}
		else
		{
			GEngineFrameTiming.FrameIntervalMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.FrameIntervalMilliseconds, FrameTimeMS);
			GEngineFrameTiming.GameThreadWorkMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.GameThreadWorkMilliseconds, GameThreadWorkMS);
			GEngineFrameTiming.RenderSyncWaitMilliseconds = SmoothFrameMetric(
				GEngineFrameTiming.RenderSyncWaitMilliseconds, RenderSyncWaitMS);
		}
		GAverageMS = GEngineFrameTiming.FrameIntervalMilliseconds;
		GAverageFPS = GAverageMS > 0.0f ? 1000.0f / GAverageMS : 0.0f;
	}

	auto GetEngineFrameTiming() -> const FEngineFrameTiming&
	{
		return GEngineFrameTiming;
	}
}

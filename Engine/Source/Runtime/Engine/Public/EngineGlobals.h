#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FEngineFrameTiming final
	{
		float FrameIntervalMilliseconds = 0.0f;
		// Main-thread elapsed times for explicit call ranges, including waits within them.
		float EngineTickMilliseconds = 0.0f;
		float UITickMilliseconds = 0.0f;
		float RenderSubmissionMilliseconds = 0.0f;
		float RenderSyncWaitMilliseconds = 0.0f;
	};

	extern ENGINE_API float GAverageFPS;
	extern ENGINE_API float GAverageMS;
	ENGINE_API auto RecordEngineFrameTickTimings(float EngineTickMilliseconds, float UITickMilliseconds) -> void;
	ENGINE_API auto RecordEngineFrameRenderTimings(float SubmissionMilliseconds, float SyncWaitMilliseconds) -> void;
	ENGINE_API auto CalculateFPSTimings() -> void;
	ENGINE_API auto GetEngineFrameTiming() -> const FEngineFrameTiming&;
} // namespace Durin

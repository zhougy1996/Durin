#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FEngineFrameTiming final
	{
		float FrameIntervalMilliseconds = 0.0f;
		float GameThreadWorkMilliseconds = 0.0f;
		float RenderSyncWaitMilliseconds = 0.0f;
	};

	extern ENGINE_API float GAverageFPS;
	extern ENGINE_API float GAverageMS;
	ENGINE_API auto RecordEngineFrameSyncWait(float Milliseconds) -> void;
	ENGINE_API auto CalculateFPSTimings() -> void;
	ENGINE_API auto GetEngineFrameTiming() -> const FEngineFrameTiming&;
} // namespace Durin

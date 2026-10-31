#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"

namespace Durin
{
	// Controls asynchronous resource admission throughput; readiness and request
	// priority remain owned by their consumers. Mode changes apply next frame/pump.
	class FShaderPipelineCache
	{
	public:
		enum class BatchMode : uint8 { Background, Fast };
		struct FBatchBudget
		{
			uint32 Shaders;
			uint32 Pipelines;
			std::chrono::microseconds Time;
		};

		RENDERCORE_API static auto SetBatchMode(BatchMode Mode) -> void;
		RENDERCORE_API static auto GetBatchMode() -> BatchMode;
		RENDERCORE_API static auto GetBatchBudget() -> FBatchBudget;
	};
}

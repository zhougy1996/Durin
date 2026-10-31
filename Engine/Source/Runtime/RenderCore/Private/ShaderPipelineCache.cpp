#include "ShaderPipelineCache.h"
#include "Logging/LogMacros.h"

namespace Durin
{
	namespace
	{
		std::atomic<FShaderPipelineCache::BatchMode> CurrentMode{FShaderPipelineCache::BatchMode::Background};
	}

	auto FShaderPipelineCache::SetBatchMode(BatchMode Mode) -> void
	{
		require(Mode == BatchMode::Background || Mode == BatchMode::Fast);
		if (CurrentMode.exchange(Mode, std::memory_order_acq_rel) != Mode)
			DURIN_DEBUG_CATEGORY("Startup", "Shader pipeline batch mode: {}", Mode == BatchMode::Fast ? "Fast" : "Background");
	}

	auto FShaderPipelineCache::GetBatchMode() -> BatchMode
	{
		return CurrentMode.load(std::memory_order_acquire);
	}

	auto FShaderPipelineCache::GetBatchBudget() -> FBatchBudget
	{
		return GetBatchMode() == BatchMode::Fast
			? FBatchBudget{32, 64, std::chrono::milliseconds(8)}
			: FBatchBudget{8, 16, std::chrono::milliseconds(2)};
	}
}

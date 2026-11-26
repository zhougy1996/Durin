#pragma once

#include "CoreMinimal.h"

#include "PipelineStateCache.h"
#include "Backend/RHIPipelineCompilation.h"

// Backend implementation seam. Do not include from renderer or feature code.
namespace Durin
{
	class FRHIPipelineStateCacheBackend final
	{
	public:
		static auto GetGraphicsPipeline(const FPipelineCompilation& Compilation) -> FGraphicsPipelineStateRHIRef { return Compilation.GetGraphicsPipeline(); }
		static auto GetComputePipeline(const FPipelineCompilation& Compilation) -> FComputePipelineStateRHIRef { return Compilation.GetComputePipeline(); }
		static auto GetGraphicsPipeline(const FPipelineState& Pipeline) -> FGraphicsPipelineStateRHIRef { return Pipeline.Compilation->GetGraphicsPipeline(); }
		static auto GetComputePipeline(const FPipelineState& Pipeline) -> FComputePipelineStateRHIRef { return Pipeline.Compilation->GetComputePipeline(); }
		RHI_API static auto WaitForRequiredPipeline(const FPipelineCompilation& Request) -> void;
		// Key must describe Initializer and be validated against this device's
		// capabilities. Cache lifecycle, payload, and independent-CPU admission still apply.
		RHI_API static auto GetGraphicsValidated(FRHIPipelineStateCache& Cache,
			const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name,
			FGraphicsPipelineStateKey Key)
			-> FGraphicsPipelineStateRef;
		RHI_API static auto GetComputeValidated(FRHIPipelineStateCache& Cache,
			const FComputePipelineStateInitializer& Initializer, std::string_view Name,
			FComputePipelineStateKey Key)
			-> FComputePipelineStateRef;
	};
}

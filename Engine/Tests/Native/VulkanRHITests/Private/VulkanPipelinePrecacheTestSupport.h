#pragma once

#include "CoreMinimal.h"

#include "PipelineStateCache.h"
#include "Backend/RHIPipelineStateCacheBackend.h"

namespace Durin
{
	struct FPipelinePrecacheTestObservation
	{
		std::shared_ptr<FPipelineState> Pipeline;
		auto IsAccepted() const -> bool { return Pipeline != nullptr; }
		auto Wait() const -> ERHIPipelineState { require(Pipeline); return Pipeline->Wait(); }
		auto GetState() const -> ERHIPipelineState { return Pipeline->GetState(); }
		auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc> { return Pipeline->GetPipelineLayout(); }
		auto GetGraphicsPipeline() const -> FGraphicsPipelineStateRHIRef { return FRHIPipelineStateCacheBackend::GetGraphicsPipeline(*Pipeline); }
		auto GetComputePipeline() const -> FComputePipelineStateRHIRef { return FRHIPipelineStateCacheBackend::GetComputePipeline(*Pipeline); }
	};
	template<typename T>
	auto PrecachePipelineForTest(const T& Initializer, FName Name) -> FPipelinePrecacheTestObservation
	{
		auto Result = [&] {
			if constexpr (std::same_as<T, FComputePipelineStateInitializer>) return PipelineStateCache::PrecacheComputePipelineState(Initializer, Name);
			else return PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, Name);
		}();
		return {std::move(Result)};
	}
}

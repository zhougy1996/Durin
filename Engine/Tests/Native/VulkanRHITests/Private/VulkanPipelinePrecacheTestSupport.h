#pragma once

#include "PipelineStateCache.h"

namespace Durin
{
	struct FPipelinePrecacheTestObservation
	{
		std::shared_ptr<FPipelineState> Pipeline;
		auto IsAccepted() const -> bool { return Pipeline != nullptr; }
		auto Wait() const -> bool { return Pipeline && Pipeline->Wait(); }
		auto GetState() const -> ERHIPipelineRequestState { return Pipeline->GetState(); }
		auto IsComplete() const -> bool { return Pipeline->IsComplete(); }
		auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc> { return Pipeline->GetPipelineLayout(); }
		auto GetResult() const -> FRHIPipelineCreationResult
		{
			FRHIPipelineCreationResult Result{.State = GetState()};
			Result.Error = Pipeline->GetCreationError();
			if (auto Graphics = std::dynamic_pointer_cast<FGraphicsPipelineState>(Pipeline)) Result.Graphics = Graphics->GetRHIPipeline();
			else Result.Compute = std::static_pointer_cast<FComputePipelineState>(Pipeline)->GetRHIPipeline();
			return Result;
		}
	};
	template<typename T>
	auto PrecachePipelineForTest(const T& Initializer, FName Name) -> FPipelinePrecacheTestObservation
	{
		auto Result = [&] {
			if constexpr (std::same_as<T, FComputePipelineStateInitializer>) return PipelineStateCache::PrecacheComputePipelineState(Initializer, Name);
			else return PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, Name);
		}();
		return {Result ? std::shared_ptr<FPipelineState>(*Result) : nullptr};
	}
}

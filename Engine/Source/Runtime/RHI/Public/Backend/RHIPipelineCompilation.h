#pragma once

#include "CoreMinimal.h"
#include "RHIPipelineCreation.h"

namespace Durin
{
	// Handles for the same live pipeline key share compilation state and completion.
	// Dropping a handle never cancels compilation retained by the device queue.
	class FPipelineCompilation
	{
	public:
		FPipelineCompilation() = default;
		auto operator==(const FPipelineCompilation&) const -> bool = default;
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		RHI_API auto GetState() const -> ERHIPipelineState;
		// Shares only the authoritative outcome, without retaining native resources.
		RHI_API auto GetStateObservation() const -> FRHIPipelineStateObservation;
		RHI_API auto IsCompute() const -> bool;
		// Immutable recording metadata is available before the native pipeline.
		RHI_API auto GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>;
		RHI_API auto BoostPriority(EPSOPrecachePriority Priority) const -> void;
		RHI_API auto GetPriority() const -> EPSOPrecachePriority;
		// Pending waits reject replay and Core workers; asynchronous callers use completion edges.
		// Returns Pending when the current execution context cannot safely wait.
		RHI_API auto Wait() const -> ERHIPipelineState;
	private:
		RHI_API auto WaitForPublication() const -> ERHIPipelineState;
		RHI_API auto GetGraphicsPipeline() const -> FGraphicsPipelineStateRHIRef;
		RHI_API auto GetComputePipeline() const -> FComputePipelineStateRHIRef;
		RHI_API auto CanWait() const -> bool;
		friend class FRHIPipelineStateCacheBackend;
		struct FState;
		std::shared_ptr<FState> State;
		friend class FPipelineCompileQueue;
	};

}

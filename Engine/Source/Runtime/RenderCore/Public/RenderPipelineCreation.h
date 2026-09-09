#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"
#include "RHIPipelineCreation.h"

namespace Durin
{
	struct FRenderResourceGeneration;
	// A consuming preparation boundary can join required first-use candidates in
	// batches before authoring a graph. Compatible refreshes do not join this batch.
	class RENDERCORE_API FRenderPipelinePreparationBatch
	{
	public:
		FRenderPipelinePreparationBatch();
		~FRenderPipelinePreparationBatch();
		FRenderPipelinePreparationBatch(const FRenderPipelinePreparationBatch&) = delete;
		auto Wait() -> bool;
		static auto HasPending() -> bool;
		static auto Add(const FRHIPipelineCreationRequest& Request) -> void;
	private:
		FRenderPipelinePreparationBatch* Previous;
		std::vector<FRHIPipelineCreationRequest> Requests;
	};

	// Retains independently cancelable PSO observations for one transactional slot.
	class RENDERCORE_API FRenderPipelineRequests
	{
	public:
		FRenderPipelineRequests();
		~FRenderPipelineRequests();
		FRenderPipelineRequests(FRenderPipelineRequests&&) noexcept;
		auto operator=(FRenderPipelineRequests&&) noexcept -> FRenderPipelineRequests&;
		auto Reset() -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
		friend class FRenderPipelineRequestScope;
	};

	// Nested resource slots each capture their own requests. A Pending candidate
	// leaves the old complete payload available and does not consume a failure retry.
	class RENDERCORE_API FRenderPipelineRequestScope
	{
	public:
		FRenderPipelineRequestScope(FRenderPipelineRequests& Requests, const FRenderResourceGeneration& Generation, bool RequiresFirstUse = true);
		~FRenderPipelineRequestScope();
		FRenderPipelineRequestScope(const FRenderPipelineRequestScope&) = delete;
		auto HasPending() const -> bool { return bPending; }
		auto MarkPending() -> void { bPending = true; }
		static auto Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> FGraphicsPipelineStateRHIRef;
		static auto Compute(FName Name, const FComputePipelineStateInitializer& Initializer) -> FComputePipelineStateRHIRef;
	private:
		FRenderPipelineRequests& Requests;
		FRenderPipelineRequestScope* Previous;
		const FRenderResourceGeneration& Generation;
		bool bPending = false;
		bool bRequiresFirstUse;

	};
}

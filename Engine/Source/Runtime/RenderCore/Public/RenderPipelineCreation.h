#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"
#include "PipelineStateCache.h"

namespace Durin
{
	struct FRenderResourceGeneration;
	enum class ERenderPipelinePreparationWait : uint8
	{
		Empty, Ready, Failed, WaitUnavailable, CapacityExceeded
	};

	// A consuming preparation boundary joins required Pending candidates, including
	// replacements, in batches before authoring a graph.
	class RENDERCORE_API FRenderPipelinePreparationBatch
	{
	public:
		FRenderPipelinePreparationBatch();
		~FRenderPipelinePreparationBatch();
		FRenderPipelinePreparationBatch(const FRenderPipelinePreparationBatch&) = delete;
		auto Wait() -> ERenderPipelinePreparationWait;
		auto GetRequestCount() const -> size_t { return Requests.size(); }
		static constexpr size_t MaximumRequests = 4096;
		static auto HasPending() -> bool;
		static auto Add(const std::shared_ptr<FPipelineState>& Pipeline) -> void;
	private:
		FRenderPipelinePreparationBatch* Previous;
		bool bCapacityExceeded = false;
		std::vector<std::shared_ptr<FPipelineState>> Requests;
	};

	// Retains shared cache identities for one transactional slot.
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
	// exposes no payload and does not consume a failure retry.
	class RENDERCORE_API FRenderPipelineRequestScope
	{
	public:
		FRenderPipelineRequestScope(FRenderPipelineRequests& Requests, const FRenderResourceGeneration& Generation);
		~FRenderPipelineRequestScope();
		FRenderPipelineRequestScope(const FRenderPipelineRequestScope&) = delete;
		auto HasPending() const -> bool { return bPending; }
		auto GetFailure() const -> const FRHICreationError& { return Failure; }
		auto MarkPending() -> void { bPending = true; }
		static auto Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> FGraphicsPipelineStateRHIRef;
		static auto Compute(FName Name, const FComputePipelineStateInitializer& Initializer) -> FComputePipelineStateRHIRef;
	private:
		FRenderPipelineRequests& Requests;
		FRenderPipelineRequestScope* Previous;
		const FRenderResourceGeneration& Generation;
		FRHICreationError Failure;
		bool bPending = false;

	};
}

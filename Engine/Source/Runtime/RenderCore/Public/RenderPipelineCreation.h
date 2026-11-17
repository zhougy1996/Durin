#pragma once

#include "CoreMinimal.h"

#include "RenderCoreAPI.h"
#include "RHIResources.h"
#include "PipelineStateCache.h"

namespace Durin
{
	struct FRenderResourceGeneration;
	// A retained CPU preparation reports completion (including failure/cancel) or
	// invalidation by leaving Pending. Polling never submits or joins work.
	class FRenderPreparationDependency
	{
	public:
		virtual ~FRenderPreparationDependency() = default;
		virtual auto IsPending() const -> bool = 0;
	};
	// Collects resource readiness and shared PSO handles without joining tasks.
	class RENDERCORE_API FRenderPipelinePreparationBatch
	{
	public:
		FRenderPipelinePreparationBatch();
		~FRenderPipelinePreparationBatch();
		FRenderPipelinePreparationBatch(const FRenderPipelinePreparationBatch&) = delete;
		auto CollectRequests(std::vector<std::shared_ptr<FPipelineState>>& Out) const -> void;
		auto HasResourcePending() const -> bool { return bResourcePending; }
		auto GetPipelineCount() const -> size_t { return PipelineStates.size(); }
		static auto HasPending() -> bool;
		static auto MarkResourcePending() -> void;
		static auto Add(const std::shared_ptr<FPipelineState>& Pipeline) -> void;
	private:
		FRenderPipelinePreparationBatch* Previous;
		bool bResourcePending = false;
		std::vector<std::shared_ptr<FPipelineState>> PipelineStates;
	};

	// Retains shared cache identities for one transactional slot.
	class FRenderPipelineStateSet
	{
	public:
		RENDERCORE_API FRenderPipelineStateSet();
		RENDERCORE_API ~FRenderPipelineStateSet();
		RENDERCORE_API FRenderPipelineStateSet(FRenderPipelineStateSet&&) noexcept;
		RENDERCORE_API auto operator=(FRenderPipelineStateSet&&) noexcept -> FRenderPipelineStateSet&;
		RENDERCORE_API auto Reset() -> void;
		RENDERCORE_API auto CollectPreparationRequests() const -> void;
		RENDERCORE_API auto HasFailed() const -> bool;
		RENDERCORE_API auto CanDeferAttempt(const FRenderResourceGeneration& Generation) const -> bool;
	private:
		struct FState;
		std::unique_ptr<FState> State;
		friend class FRenderPipelinePreparationScope;
	};

	// Nested resource slots each capture their own requests. A Pending candidate
	// exposes no payload and does not consume a failure retry.
	class RENDERCORE_API FRenderPipelinePreparationScope
	{
	public:
		FRenderPipelinePreparationScope(FRenderPipelineStateSet& PipelineStates, const FRenderResourceGeneration& Generation);
		~FRenderPipelinePreparationScope();
		FRenderPipelinePreparationScope(const FRenderPipelinePreparationScope&) = delete;
		auto HasPending() const -> bool { return bPending; }
		auto MarkPending() -> void { bPending = true; }
		auto PropagatePending(bool bResourcePending) -> void;
		// CPU preparation is owned only until this resource attempt publishes or resets.
		static auto RetainPreparation(std::shared_ptr<const FRenderPreparationDependency> Preparation) -> void;
		static auto MarkResourcePending() -> void;
		auto ReleasePreparations() -> void;
		static auto Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> FGraphicsPipelineStateRef;
		static auto Compute(FName Name, const FComputePipelineStateInitializer& Initializer) -> FComputePipelineStateRef;
	private:
		FRenderPipelineStateSet& PipelineStates;
		FRenderPipelinePreparationScope* Previous;
		const FRenderResourceGeneration& Generation;
		bool bPending = false;

	};
}

#include "RenderPipelineCreation.h"
#include "RenderResourceCreation.h"

namespace Durin
{
	static thread_local FRenderPipelinePreparationBatch* Preparation = nullptr;
	FRenderPipelinePreparationBatch::FRenderPipelinePreparationBatch(bool bInPrecachePipelines)
		: Previous(Preparation), bPrecachePipelines(bInPrecachePipelines) { Preparation = this; }
	FRenderPipelinePreparationBatch::~FRenderPipelinePreparationBatch() { Preparation = Previous; }
	auto FRenderPipelinePreparationBatch::ShouldPrecachePipelines() -> bool { return Preparation && Preparation->bPrecachePipelines; }
	auto FRenderPipelinePreparationBatch::HasPending() -> bool
	{
		return Preparation && (Preparation->bResourcePending || std::ranges::any_of(Preparation->PipelineStates,
			[](const auto& Request) { return Request->GetState() == ERHIPipelineState::Pending; }));
	}
	auto FRenderPipelinePreparationBatch::MarkResourcePending() -> void { if (Preparation) Preparation->bResourcePending = true; }
	auto FRenderPipelinePreparationBatch::Add(const std::shared_ptr<FPipelineState>& Pipeline) -> void
	{
		if (!Preparation || std::ranges::contains(Preparation->PipelineStates, Pipeline)) return;
		Preparation->PipelineStates.push_back(Pipeline);
	}
	auto FRenderPipelinePreparationBatch::CollectRequests(std::vector<std::shared_ptr<FPipelineState>>& Out) const -> void
	{
		for (const auto& Request : PipelineStates)
		{
			if (std::ranges::contains(Out, Request)) continue;
			Out.push_back(Request);
		}
	}
	auto FRenderResourceAttemptState::CanDeferAttempt(const FRenderResourceGeneration& CurrentGeneration) const -> bool
	{
		if (Generation != CurrentGeneration) return false;
		bool bPending = false;
		for (const auto& Dependency : RetryDependencies)
		{
			const bool bStillPending = Dependency.Owner->IsPending();
			if (bStillPending != Dependency.bWasPending) return false;
			bPending |= bStillPending;
		}
		return bPending;
	}
	static thread_local FRenderPipelinePreparationScope* Current = nullptr;
	FRenderPipelinePreparationScope::FRenderPipelinePreparationScope(FRenderResourceAttemptState& InAttempt,
		const FRenderResourceGeneration& Generation)
		: Attempt(InAttempt), Previous(Current)
	{
		if (Attempt.Generation != Generation) Attempt.Reset();
		Attempt.Generation = Generation;
		for (auto& Dependency : Attempt.RetryDependencies) Dependency.bObserved = false;
		Current = this;
	}
	FRenderPipelinePreparationScope::~FRenderPipelinePreparationScope()
	{
		std::erase_if(Attempt.RetryDependencies, [](const auto& Dependency) { return !Dependency.bObserved; });
		Current = Previous;
	}
	auto FRenderPipelinePreparationScope::RetainPreparation(std::shared_ptr<const FRenderPreparationDependency> Preparation) -> void
	{
		if (!Current || !Preparation) return;
		auto& Dependencies = Current->Attempt.RetryDependencies;
		const bool bPending = Preparation->IsPending();
		auto Existing = std::ranges::find(Dependencies, Preparation, &FRenderResourceAttemptState::FDependency::Owner);
		if (Existing == Dependencies.end()) Dependencies.push_back({std::move(Preparation), bPending, true});
		else { Existing->bWasPending = bPending; Existing->bObserved = true; }
	}
	auto FRenderPipelinePreparationScope::ReleasePreparations() -> void { Attempt.RetryDependencies.clear(); }
	auto FRenderPipelinePreparationScope::PropagatePending(bool bResourcePending) -> void
	{
		if (Previous) Previous->MarkPending();
		if (bResourcePending) FRenderPipelinePreparationBatch::MarkResourcePending();
	}
	auto FRenderPipelinePreparationScope::MarkResourcePending() -> void
	{
		if (Current) Current->MarkPending();
		FRenderPipelinePreparationBatch::MarkResourcePending();
	}
}

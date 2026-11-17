#include "RenderPipelineCreation.h"
#include "Threading/TaskRuntime.h"
#include "Backend/RHICreation.h"
#include "RenderResourceCreation.h"
#include "DynamicRHI.h"

namespace Durin
{
	static thread_local FRenderPipelinePreparationBatch* Preparation = nullptr;
	FRenderPipelinePreparationBatch::FRenderPipelinePreparationBatch() : Previous(Preparation) { Preparation = this; }
	FRenderPipelinePreparationBatch::~FRenderPipelinePreparationBatch() { Preparation = Previous; }
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
	struct FRenderPipelineStateSet::FState
	{
		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FEntry { FKey Key; std::shared_ptr<FPipelineState> Pipeline; };
		FRenderResourceGeneration Generation;
		std::vector<FEntry> Entries;
		struct FPreparation
		{
			std::shared_ptr<const FRenderPreparationDependency> Owner;
			bool bWasPending = false;
			bool bObserved = false;
		};
		std::vector<FPreparation> Preparations;

		template<typename T> auto Request(const T& Initializer, FName Name)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TKey = std::conditional_t<Graphics, FGraphicsPipelineStateKey, FComputePipelineStateKey>;
			using TPipeline = std::conditional_t<Graphics, FGraphicsPipelineState, FComputePipelineState>;
			using TResult = std::shared_ptr<TPipeline>;
			try
			{
				auto Valid = [&] {
					if constexpr (Graphics) return BuildGraphicsPipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
					else return BuildComputePipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
				}();
				if (!Valid) return TResult{};
				auto& Key = *Valid;
				for (const auto& Entry : Entries)
					if (const auto* Existing = std::get_if<TKey>(&Entry.Key); Existing && *Existing == Key) return TResult(std::static_pointer_cast<TPipeline>(Entry.Pipeline));
				TResult Result;
				if constexpr (Graphics) Result = PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, Name);
				else Result = PipelineStateCache::PrecacheComputePipelineState(Initializer, Name);
				if (Result)
				{
					Entries.push_back({std::move(Key), Result});
				}
				return Result;
			}
			catch (const std::bad_alloc&)
			{
				RHITerminateOnOutOfMemory("render pipeline request");
			}
		}
	};

	FRenderPipelineStateSet::FRenderPipelineStateSet() = default;
	FRenderPipelineStateSet::~FRenderPipelineStateSet() = default;
	FRenderPipelineStateSet::FRenderPipelineStateSet(FRenderPipelineStateSet&&) noexcept = default;
	auto FRenderPipelineStateSet::operator=(FRenderPipelineStateSet&&) noexcept -> FRenderPipelineStateSet& = default;
	auto FRenderPipelineStateSet::Reset() -> void { State.reset(); }
	auto FRenderPipelineStateSet::CollectPreparationRequests() const -> void
	{
		if (!State) return;
		for (const auto& Entry : State->Entries)
			FRenderPipelinePreparationBatch::Add(Entry.Pipeline);
	}
	auto FRenderPipelineStateSet::HasFailed() const -> bool
	{
		return State && std::ranges::any_of(State->Entries, [](const auto& Entry) {
			const auto Status = Entry.Pipeline->GetState();
			return Status == ERHIPipelineState::Failed || Status == ERHIPipelineState::Canceled;
		});
	}
	auto FRenderPipelineStateSet::CanDeferAttempt(const FRenderResourceGeneration& Generation) const -> bool
	{
		if (!State || State->Generation != Generation) return false;
		bool bPending = false;
		for (const auto& Preparation : State->Preparations)
		{
			const bool bStillPending = Preparation.Owner->IsPending();
			if (bStillPending != Preparation.bWasPending) return false;
			bPending |= bStillPending;
		}
		return bPending;
	}
	static thread_local FRenderPipelinePreparationScope* Current = nullptr;
	FRenderPipelinePreparationScope::FRenderPipelinePreparationScope(FRenderPipelineStateSet& InPipelineStates,
		const FRenderResourceGeneration& InGeneration)
		: PipelineStates(InPipelineStates), Previous(Current), Generation(InGeneration)
	{
		if (PipelineStates.State && PipelineStates.State->Generation != Generation) PipelineStates.Reset();
		if (PipelineStates.State)
			for (auto& Preparation : PipelineStates.State->Preparations) Preparation.bObserved = false;
		Current = this;
	}
	FRenderPipelinePreparationScope::~FRenderPipelinePreparationScope()
	{
		if (PipelineStates.State)
			std::erase_if(PipelineStates.State->Preparations, [](const auto& Preparation) { return !Preparation.bObserved; });
		Current = Previous;
	}
	auto FRenderPipelinePreparationScope::RetainPreparation(std::shared_ptr<const FRenderPreparationDependency> Preparation) -> void
	{
		if (!Current || !Preparation) return;
		if (!Current->PipelineStates.State)
		{
			Current->PipelineStates.State = std::make_unique<FRenderPipelineStateSet::FState>();
			Current->PipelineStates.State->Generation = Current->Generation;
		}
		auto& Owners = Current->PipelineStates.State->Preparations;
		const bool bPending = Preparation->IsPending();
		auto Existing = std::ranges::find(Owners, Preparation, &FRenderPipelineStateSet::FState::FPreparation::Owner);
		if (Existing == Owners.end()) Owners.push_back({std::move(Preparation), bPending, true});
		else { Existing->bWasPending = bPending; Existing->bObserved = true; }
	}
	auto FRenderPipelinePreparationScope::ReleasePreparations() -> void
	{
		if (PipelineStates.State) PipelineStates.State->Preparations.clear();
	}
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

	auto FRenderPipelinePreparationScope::Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer)
		-> FGraphicsPipelineStateRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->PipelineStates.State)
		{
			Current->PipelineStates.State = std::make_unique<FRenderPipelineStateSet::FState>();
			Current->PipelineStates.State->Generation = Current->Generation;
		}
		auto Request = Current->PipelineStates.State->Request(Initializer, Name);
		if (!Request) return nullptr;
		const auto& Pipeline = Request;
		const auto Status = Pipeline->GetState();
		FRenderPipelinePreparationBatch::Add(Pipeline);
		if (Status == ERHIPipelineState::Failed || Status == ERHIPipelineState::Canceled)
		{
			return nullptr;
		}
		return Pipeline;
	}
	auto FRenderPipelinePreparationScope::Compute(FName Name, const FComputePipelineStateInitializer& Initializer)
		-> FComputePipelineStateRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->PipelineStates.State)
		{
			Current->PipelineStates.State = std::make_unique<FRenderPipelineStateSet::FState>();
			Current->PipelineStates.State->Generation = Current->Generation;
		}
		auto Request = Current->PipelineStates.State->Request(Initializer, Name);
		if (!Request) return nullptr;
		const auto& Pipeline = Request;
		const auto Status = Pipeline->GetState();
		FRenderPipelinePreparationBatch::Add(Pipeline);
		if (Status == ERHIPipelineState::Failed || Status == ERHIPipelineState::Canceled)
		{
			return nullptr;
		}
		return Pipeline;
	}
}

#include "RenderPipelineCreation.h"
#include "RenderResourceCreation.h"
#include "DynamicRHI.h"

namespace Durin
{
	static thread_local FRenderPipelinePreparationBatch* Preparation = nullptr;
	FRenderPipelinePreparationBatch::FRenderPipelinePreparationBatch() : Previous(Preparation) { Preparation = this; }
	FRenderPipelinePreparationBatch::~FRenderPipelinePreparationBatch() { Preparation = Previous; }
	auto FRenderPipelinePreparationBatch::HasPending() -> bool { return Preparation && !Preparation->Requests.empty(); }
	auto FRenderPipelinePreparationBatch::Add(const std::shared_ptr<FPipelineState>& Pipeline) -> void
	{
		if (!Preparation || std::ranges::contains(Preparation->Requests, Pipeline)) return;
		if (Preparation->Requests.size() == MaximumRequests)
		{
			Preparation->bCapacityExceeded = true;
			return;
		}
		Preparation->Requests.push_back(Pipeline);
	}
	auto FRenderPipelinePreparationBatch::Wait() -> ERenderPipelinePreparationWait
	{
		if (bCapacityExceeded) return ERenderPipelinePreparationWait::CapacityExceeded;
		if (Requests.empty()) return ERenderPipelinePreparationWait::Empty;
		for (const auto& Request : Requests)
			if (!Request->CanWait()) return ERenderPipelinePreparationWait::WaitUnavailable;
		bool bReady = true;
		for (const auto& Request : Requests)
		{
			(void)Request->Wait();
			if (Request->GetState() == ERHIPipelineRequestState::Pending)
				return ERenderPipelinePreparationWait::WaitUnavailable;
			(void)Request->GetCreationError(); // Preserve terminal creator exceptions.
			bReady = Request->GetState() == ERHIPipelineRequestState::Ready && bReady;
		}
		return bReady ? ERenderPipelinePreparationWait::Ready : ERenderPipelinePreparationWait::Failed;
	}
	struct FRenderPipelineRequests::FState
	{
		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FEntry { FKey Key; std::shared_ptr<FPipelineState> Pipeline; };
		FRenderResourceGeneration Generation;
		std::vector<FEntry> Entries;
		uint64 KeyBytes = 0;

		template<typename T> auto Request(const T& Initializer, FName Name, bool& RetryAdmission)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TKey = std::conditional_t<Graphics, FGraphicsPipelineStateKey, FComputePipelineStateKey>;
			using TPipeline = std::conditional_t<Graphics, FGraphicsPipelineState, FComputePipelineState>;
			using TResult = std::expected<std::shared_ptr<TPipeline>, ERHIPipelineRequestRejection>;
			if (!IsPipelineCreationPayloadBounded(Initializer, Name.ToString()))
				return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
			auto Valid = [&] {
				if constexpr (Graphics) return BuildGraphicsPipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
				else return BuildComputePipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
			}();
			if (!Valid) return TResult(std::unexpected(ERHIPipelineRequestRejection::InvalidDescription));
			auto& Key = *Valid;
			for (const auto& Entry : Entries)
				if (const auto* Existing = std::get_if<TKey>(&Entry.Key); Existing && *Existing == Key) return TResult(std::static_pointer_cast<TPipeline>(Entry.Pipeline));
			uint64 Bytes = sizeof(FEntry);
			Bytes += Key.PipelineLayout.BindingLayouts.capacity() * sizeof(FBindingLayout);
			Bytes += Key.PipelineLayout.PushConstantRanges.capacity() * sizeof(FPushConstantRange);
			for (const auto& Layout : Key.PipelineLayout.BindingLayouts) Bytes += Layout.BindingLayouts.capacity() * sizeof(FBindingLayoutItem);
			if constexpr (Graphics)
			{
				Bytes += Key.VertexElements.capacity() * sizeof(FRHIVertexElementIdentity);
				Bytes += Key.ColorBlendStates.capacity() * sizeof(FRHIColorBlendState);
			}
			if (Entries.size() >= 256 || Bytes > 1024 * 1024 - KeyBytes)
				return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
			TResult Result;
			if constexpr (Graphics) Result = PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, Name);
			else Result = PipelineStateCache::PrecacheComputePipelineState(Initializer, Name);
			RetryAdmission = !Result && Result.error() == ERHIPipelineRequestRejection::CapacityExceeded;
			if (Result)
			{
				Entries.push_back({std::move(Key), *Result});
				KeyBytes += Bytes;
			}
			return Result;
		}
	};

	FRenderPipelineRequests::FRenderPipelineRequests() = default;
	FRenderPipelineRequests::~FRenderPipelineRequests() = default;
	FRenderPipelineRequests::FRenderPipelineRequests(FRenderPipelineRequests&&) noexcept = default;
	auto FRenderPipelineRequests::operator=(FRenderPipelineRequests&&) noexcept -> FRenderPipelineRequests& = default;
	auto FRenderPipelineRequests::Reset() -> void { State.reset(); }
	static thread_local FRenderPipelineRequestScope* Current = nullptr;
	FRenderPipelineRequestScope::FRenderPipelineRequestScope(FRenderPipelineRequests& InRequests,
		const FRenderResourceGeneration& InGeneration)
		: Requests(InRequests), Previous(Current), Generation(InGeneration)
	{
		if (Requests.State && Requests.State->Generation != Generation) Requests.Reset();
		Current = this;
	}
	FRenderPipelineRequestScope::~FRenderPipelineRequestScope() { Current = Previous; }
	auto FRenderPipelineRequestScope::Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer)
		-> FGraphicsPipelineStateRHIRef
	{
		if (!Current || !IsTaskSchedulerRunning()) return GDynamicRHI->RHICreateGraphicsPipelineState(Name, Initializer);
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_unique<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		bool RetryAdmission = false;
		auto Request = Current->Requests.State->Request(Initializer, Name, RetryAdmission);
		if (RetryAdmission) Current->MarkPending();
		if (!Request) return nullptr;
		const auto& Pipeline = *Request;
		const auto Status = Pipeline->GetState();
		if (Status == ERHIPipelineRequestState::Pending)
		{
			Current->MarkPending();
			FRenderPipelinePreparationBatch::Add(Pipeline);
		}
		const auto Error = Pipeline->GetCreationError();
		if (Error.HasError() && !Current->Failure.HasError()) Current->Failure = Error;
		return Pipeline->GetRHIPipeline();
	}
	auto FRenderPipelineRequestScope::Compute(FName Name, const FComputePipelineStateInitializer& Initializer)
		-> FComputePipelineStateRHIRef
	{
		if (!Current || !IsTaskSchedulerRunning()) return GDynamicRHI->RHICreateComputePipelineState(Name, Initializer);
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_unique<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		bool RetryAdmission = false;
		auto Request = Current->Requests.State->Request(Initializer, Name, RetryAdmission);
		if (RetryAdmission) Current->MarkPending();
		if (!Request) return nullptr;
		const auto& Pipeline = *Request;
		const auto Status = Pipeline->GetState();
		if (Status == ERHIPipelineRequestState::Pending)
		{
			Current->MarkPending();
			FRenderPipelinePreparationBatch::Add(Pipeline);
		}
		const auto Error = Pipeline->GetCreationError();
		if (Error.HasError() && !Current->Failure.HasError()) Current->Failure = Error;
		return Pipeline->GetRHIPipeline();
	}
}

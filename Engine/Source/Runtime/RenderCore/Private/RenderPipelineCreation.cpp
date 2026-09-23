#include "RenderPipelineCreation.h"
#include "RenderResourceCreation.h"
#include "DynamicRHI.h"

namespace Durin
{
	static thread_local FRenderPipelinePreparationBatch* Preparation = nullptr;
	FRenderPipelinePreparationBatch::FRenderPipelinePreparationBatch() : Previous(Preparation) { Preparation = this; }
	FRenderPipelinePreparationBatch::~FRenderPipelinePreparationBatch() { Preparation = Previous; }
	auto FRenderPipelinePreparationBatch::HasPending() -> bool { return Preparation && !Preparation->Requests.empty(); }
	auto FRenderPipelinePreparationBatch::Add(const FRHIPipelineCreationRequest& Request) -> void
	{
		if (!Preparation || std::ranges::contains(Preparation->Requests, Request)) return;
		if (Preparation->Requests.size() == MaximumRequests)
		{
			Preparation->bCapacityExceeded = true;
			return;
		}
		Preparation->Requests.push_back(Request);
	}
	auto FRenderPipelinePreparationBatch::Wait() -> ERenderPipelinePreparationWait
	{
		if (bCapacityExceeded) return ERenderPipelinePreparationWait::CapacityExceeded;
		if (Requests.empty()) return ERenderPipelinePreparationWait::Empty;
		for (const auto& Request : Requests)
			if (!Request.CanWait()) return ERenderPipelinePreparationWait::WaitUnavailable;
		bool bReady = true;
		for (const auto& Request : Requests)
		{
			(void)Request.Wait();
			if (Request.GetState() == ERHIPipelineRequestState::Pending)
				return ERenderPipelinePreparationWait::WaitUnavailable;
			bReady = Request.GetResult().State == ERHIPipelineRequestState::Ready && bReady;
		}
		return bReady ? ERenderPipelinePreparationWait::Ready : ERenderPipelinePreparationWait::Failed;
	}
	struct FRenderPipelineRequests::FState
	{
		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FEntry { FKey Key; FRHIPipelineCreationRequest Request; };
		FRenderResourceGeneration Generation;
		std::vector<FEntry> Entries;
		uint64 KeyBytes = 0;

		~FState() { for (const auto& Entry : Entries) Entry.Request.Cancel(); }
		template<typename T> auto Request(const T& Initializer, FName Name, bool& RetryAdmission) -> FRHIPipelineCreationRequest
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TKey = std::conditional_t<Graphics, FGraphicsPipelineStateKey, FComputePipelineStateKey>;
			if (!IsPipelineCreationPayloadBounded(Initializer, Name.ToString()))
				return FRHIPipelineCreationRequest::Rejected(ERHIPipelineRequestRejection::CapacityExceeded);
			auto Valid = [&] {
				if constexpr (Graphics) return BuildGraphicsPipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
				else return BuildComputePipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
			}();
			if (!Valid) return FRHIPipelineCreationRequest::Rejected(ERHIPipelineRequestRejection::InvalidDescription);
			auto& Key = *Valid;
			for (const auto& Entry : Entries)
				if (const auto* Existing = std::get_if<TKey>(&Entry.Key); Existing && *Existing == Key) return Entry.Request;
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
				return FRHIPipelineCreationRequest::Rejected(ERHIPipelineRequestRejection::CapacityExceeded);
			FRHIPipelineCreationRequest Result;
			if constexpr (Graphics) Result = GDynamicRHI->RHIRequestGraphicsPipelineState(Initializer, Name.ToString());
			else Result = GDynamicRHI->RHIRequestComputePipelineState(Initializer, Name.ToString());
			RetryAdmission = !Result.IsAccepted() && Result.GetRejection() == ERHIPipelineRequestRejection::CapacityExceeded;
			if (Result.IsAccepted())
			{
				Entries.push_back({std::move(Key), Result});
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
		const FRenderResourceGeneration& InGeneration, bool RequiresFirstUse)
		: Requests(InRequests), Previous(Current), Generation(InGeneration), bRequiresFirstUse(RequiresFirstUse)
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
		if (!Request.IsAccepted()) return nullptr;
		auto Result = Request.GetResult();
		if (Result.State == ERHIPipelineRequestState::Pending)
		{
			Current->MarkPending();
			if (Current->bRequiresFirstUse) FRenderPipelinePreparationBatch::Add(Request);
		}
		if (Result.Error.HasError() && !Current->Failure.HasError()) Current->Failure = Result.Error;
		return Result.Graphics;
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
		if (!Request.IsAccepted()) return nullptr;
		auto Result = Request.GetResult();
		if (Result.State == ERHIPipelineRequestState::Pending)
		{
			Current->MarkPending();
			if (Current->bRequiresFirstUse) FRenderPipelinePreparationBatch::Add(Request);
		}
		if (Result.Error.HasError() && !Current->Failure.HasError()) Current->Failure = Result.Error;
		return Result.Compute;
	}
}

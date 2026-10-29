#include "RenderPipelineCreation.h"
#include "RenderResourceCreation.h"
#include "DynamicRHI.h"
#include "RenderResourcePreparation.h"

namespace Durin
{
	static thread_local FRenderPipelinePreparationBatch* Preparation = nullptr;
	FRenderPipelinePreparationBatch::FRenderPipelinePreparationBatch() : Previous(Preparation) { Preparation = this; }
	FRenderPipelinePreparationBatch::~FRenderPipelinePreparationBatch() { Preparation = Previous; }
	auto FRenderPipelinePreparationBatch::HasPending() -> bool
	{
		return Preparation && (Preparation->bResourcePending || !Preparation->Requests.empty());
	}
	auto FRenderPipelinePreparationBatch::MarkResourcePending() -> void { if (Preparation) Preparation->bResourcePending = true; }
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
		if (bResourcePending) return ERenderPipelinePreparationWait::WaitUnavailable;
		if (Requests.empty()) return ERenderPipelinePreparationWait::Empty;
		for (const auto& Request : Requests)
			if (!Request->CanWait()) return ERenderPipelinePreparationWait::WaitUnavailable;
		bool bReady = true;
		for (const auto& Request : Requests)
		{
			(void)Request->Wait();
			if (Request->GetState() == ERHIPipelineRequestState::Pending)
				return ERenderPipelinePreparationWait::WaitUnavailable;
			(void)Request->GetResult(); // Preserve terminal creator exceptions.
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
		std::vector<std::shared_ptr<const void>> Preparations;
		uint64 KeyBytes = 0;

		template<typename T> auto Request(const T& Initializer, FName Name, bool& RetryAdmission)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TKey = std::conditional_t<Graphics, FGraphicsPipelineStateKey, FComputePipelineStateKey>;
			using TPipeline = std::conditional_t<Graphics, FGraphicsPipelineState, FComputePipelineState>;
			using TResult = std::expected<std::shared_ptr<TPipeline>, ERHIPipelineRequestRejection>;
			try
			{
				char NameBuffer[FName::StringBufferSize];
				std::string_view NameView = Name.GetPlainNameView();
				if (Name.HasNumber())
				{
					size_t Length = 0;
					require(Name.TryWriteString(NameBuffer, Length));
					NameView = std::string_view(NameBuffer, Length);
				}
				if (!IsPipelineCreationPayloadBounded(Initializer, NameView))
					return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
				auto Valid = [&] {
					if constexpr (Graphics) return BuildGraphicsPipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
					else return BuildComputePipelineStateKey(Initializer, GDynamicRHI->RHIGetCapabilities());
				}();
				if (!Valid)
				{
					RetryAdmission = Valid.error() == std::remove_cvref_t<decltype(Valid.error())>::CapacityExceeded;
					return TResult(std::unexpected(RetryAdmission ? ERHIPipelineRequestRejection::CapacityExceeded
						: ERHIPipelineRequestRejection::InvalidDescription));
				}
				auto& Key = *Valid;
				for (const auto& Entry : Entries)
					if (const auto* Existing = std::get_if<TKey>(&Entry.Key); Existing && *Existing == Key) return TResult(std::static_pointer_cast<TPipeline>(Entry.Pipeline));
				const uint64 Bytes = sizeof(FEntry) - sizeof(TKey) + GetPipelineKeyOwnedBytes(Key);
				if (Entries.size() >= 256 || Bytes > 1024 * 1024 - KeyBytes)
					return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
				if (!FRenderResourcePreparationScope::TryBeforePipeline())
				{ RetryAdmission = true; return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded)); }
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
			catch (const std::bad_alloc&) { RetryAdmission = true; return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded)); }
		}
	};

	FRenderPipelineRequests::FRenderPipelineRequests() = default;
	FRenderPipelineRequests::~FRenderPipelineRequests() = default;
	FRenderPipelineRequests::FRenderPipelineRequests(FRenderPipelineRequests&&) noexcept = default;
	auto FRenderPipelineRequests::operator=(FRenderPipelineRequests&&) noexcept -> FRenderPipelineRequests& = default;
	auto FRenderPipelineRequests::Reset() -> void { State.reset(); }
	auto FRenderPipelineRequests::RetainForPreparation() const -> void
	{
		if (State) FRenderResourcePreparationScope::Retain(State, sizeof(FState) + State->KeyBytes);
	}
	static thread_local FRenderPipelineRequestScope* Current = nullptr;
	FRenderPipelineRequestScope::FRenderPipelineRequestScope(FRenderPipelineRequests& InRequests,
		const FRenderResourceGeneration& InGeneration)
		: Requests(InRequests), Previous(Current), Generation(InGeneration)
	{
		if (Requests.State && Requests.State->Generation != Generation) Requests.Reset();
		Current = this;
	}
	FRenderPipelineRequestScope::~FRenderPipelineRequestScope() { Current = Previous; }
	auto FRenderPipelineRequestScope::RetainPreparation(std::shared_ptr<const void> Preparation) -> void
	{
		if (!Current) return;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_shared<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		auto& Owners = Current->Requests.State->Preparations;
		if (!std::ranges::contains(Owners, Preparation)) Owners.push_back(std::move(Preparation));
	}
	auto FRenderPipelineRequestScope::ReleasePreparations() -> void
	{
		if (Requests.State) Requests.State->Preparations.clear();
	}
	auto FRenderPipelineRequestScope::PropagatePending(bool bResourcePending) -> void
	{
		if (Previous) Previous->MarkPending();
		if (bResourcePending) FRenderPipelinePreparationBatch::MarkResourcePending();
	}
	auto FRenderPipelineRequestScope::MarkResourcePending() -> void
	{
		if (Current) Current->MarkPending();
		FRenderPipelinePreparationBatch::MarkResourcePending();
	}

	auto FRenderPipelineRequestScope::Graphics(FName Name, const FGraphicsPipelineStateInitializer& Initializer)
		-> FGraphicsPipelineStateRHIRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_shared<FRenderPipelineRequests::FState>();
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
		return Pipeline->GetRHIPipeline();
	}
	auto FRenderPipelineRequestScope::Compute(FName Name, const FComputePipelineStateInitializer& Initializer)
		-> FComputePipelineStateRHIRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_shared<FRenderPipelineRequests::FState>();
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
		return Pipeline->GetRHIPipeline();
	}
}

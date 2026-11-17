#include "RenderPipelineCreation.h"
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
		return Preparation && (Preparation->bResourcePending || std::ranges::any_of(Preparation->Requests,
			[](const auto& Request) { return Request->GetState() == ERHIPipelineRequestState::Pending; }));
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
	auto FRenderPipelinePreparationBatch::CollectRequests(std::vector<std::shared_ptr<FPipelineState>>& Out) const -> bool
	{
		if (bCapacityExceeded) return false;
		for (const auto& Request : Requests)
		{
			if (std::ranges::contains(Out, Request)) continue;
			if (Out.size() >= MaximumRequests) return false;
			Out.push_back(Request);
		}
		return true;
	}
	struct FRenderPipelineRequests::FState
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
				TResult Result;
				if constexpr (Graphics) Result = PipelineStateCache::GetAndOrCreateGraphicsPipelineState(Initializer, Name);
				else Result = PipelineStateCache::GetAndOrCreateComputePipelineState(Initializer, Name);
				RetryAdmission = !Result && Result.error() == ERHIPipelineRequestRejection::CapacityExceeded;
				if (Result)
				{
					Entries.push_back({std::move(Key), *Result});
					KeyBytes += Bytes;
				}
				return Result;
			}
			catch (const std::bad_alloc&)
			{
				RHITerminateOnOutOfMemory("render pipeline request");
			}
		}
	};

	FRenderPipelineRequests::FRenderPipelineRequests() = default;
	FRenderPipelineRequests::~FRenderPipelineRequests() = default;
	FRenderPipelineRequests::FRenderPipelineRequests(FRenderPipelineRequests&&) noexcept = default;
	auto FRenderPipelineRequests::operator=(FRenderPipelineRequests&&) noexcept -> FRenderPipelineRequests& = default;
	auto FRenderPipelineRequests::Reset() -> void { State.reset(); }
	auto FRenderPipelineRequests::CollectPreparationRequests() const -> void
	{
		if (!State) return;
		for (const auto& Entry : State->Entries)
			FRenderPipelinePreparationBatch::Add(Entry.Pipeline);
	}
	auto FRenderPipelineRequests::HasFailed() const -> bool
	{
		return State && std::ranges::any_of(State->Entries, [](const auto& Entry) {
			const auto Status = Entry.Pipeline->GetState();
			return Status == ERHIPipelineRequestState::Failed || Status == ERHIPipelineRequestState::Canceled;
		});
	}
	auto FRenderPipelineRequests::CanDeferAttempt(const FRenderResourceGeneration& Generation) const -> bool
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
	static thread_local FRenderPipelineRequestScope* Current = nullptr;
	FRenderPipelineRequestScope::FRenderPipelineRequestScope(FRenderPipelineRequests& InRequests,
		const FRenderResourceGeneration& InGeneration)
		: Requests(InRequests), Previous(Current), Generation(InGeneration)
	{
		if (Requests.State && Requests.State->Generation != Generation) Requests.Reset();
		if (Requests.State)
			for (auto& Preparation : Requests.State->Preparations) Preparation.bObserved = false;
		Current = this;
	}
	FRenderPipelineRequestScope::~FRenderPipelineRequestScope()
	{
		if (Requests.State)
			std::erase_if(Requests.State->Preparations, [](const auto& Preparation) { return !Preparation.bObserved; });
		Current = Previous;
	}
	auto FRenderPipelineRequestScope::RetainPreparation(std::shared_ptr<const FRenderPreparationDependency> Preparation) -> void
	{
		if (!Current || !Preparation) return;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_unique<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		auto& Owners = Current->Requests.State->Preparations;
		const bool bPending = Preparation->IsPending();
		auto Existing = std::ranges::find(Owners, Preparation, &FRenderPipelineRequests::FState::FPreparation::Owner);
		if (Existing == Owners.end()) Owners.push_back({std::move(Preparation), bPending, true});
		else { Existing->bWasPending = bPending; Existing->bObserved = true; }
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
		-> FGraphicsPipelineStateRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_unique<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		bool RetryAdmission = false;
		auto Request = Current->Requests.State->Request(Initializer, Name, RetryAdmission);
		if (RetryAdmission) MarkResourcePending();
		if (!Request) return nullptr;
		const auto& Pipeline = *Request;
		const auto Status = Pipeline->GetState();
		FRenderPipelinePreparationBatch::Add(Pipeline);
		if (Status == ERHIPipelineRequestState::Failed || Status == ERHIPipelineRequestState::Canceled)
		{
			(void)Pipeline->GetResult();
			return nullptr;
		}
		return Pipeline;
	}
	auto FRenderPipelineRequestScope::Compute(FName Name, const FComputePipelineStateInitializer& Initializer)
		-> FComputePipelineStateRef
	{
		// Rendering requests never change execution mode when the host is unavailable.
		if (!Current || !IsTaskSchedulerRunning() || !GDynamicRHI) return nullptr;
		if (!Current->Requests.State)
		{
			Current->Requests.State = std::make_unique<FRenderPipelineRequests::FState>();
			Current->Requests.State->Generation = Current->Generation;
		}
		bool RetryAdmission = false;
		auto Request = Current->Requests.State->Request(Initializer, Name, RetryAdmission);
		if (RetryAdmission) MarkResourcePending();
		if (!Request) return nullptr;
		const auto& Pipeline = *Request;
		const auto Status = Pipeline->GetState();
		FRenderPipelinePreparationBatch::Add(Pipeline);
		if (Status == ERHIPipelineRequestState::Failed || Status == ERHIPipelineRequestState::Canceled)
		{
			(void)Pipeline->GetResult();
			return nullptr;
		}
		return Pipeline;
	}
}

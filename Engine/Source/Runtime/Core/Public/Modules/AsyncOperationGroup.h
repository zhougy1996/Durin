#pragma once

#include "CoreAPI.h"
#include "Misc/Name.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace Detail
	{
		struct FAsyncOperationGroupState;
		struct FModularFeatureOwnerState;
	}

	// Selects whether accepted work finishes normally or observes explicit cancellation during close.
	enum class EAsyncOperationCloseMode : uint8
	{
		Drain,
		Cancel,
	};

	// Describes the irreversible admission and quiescence lifecycle of one operation group.
	enum class EAsyncOperationGroupState : uint8
	{
		Invalid,
		Open,
		ClosingDrain,
		ClosingCancel,
		QuiescentDrain,
		QuiescentCancel,
	};

	// Records the business-visible reason cancellation was explicitly requested.
	enum class EAsyncOperationAbortReason : uint8
	{
		None,
		OwnerRequested,
		ModuleShutdown,
		Superseded,
		Failure,
	};

	// Categorizes close admission and cancellation escalation.
	enum class EAsyncOperationCloseStatus : uint8
	{
		Closed,
		EscalatedToCancel,
		AlreadyClosed,
		Invalid,
	};

	// Categorizes bounded quiescence and unsupported recursive or executor waits.
	enum class EAsyncOperationDrainStatus : uint8
	{
		Succeeded,
		TimedOut,
		SelfWait,
		UnsupportedThread,
		Open,
		Invalid,
	};

	// Defines the policy applied automatically when the owning module begins retirement.
	struct FAsyncOperationGroupOptions
	{
		EAsyncOperationCloseMode ShutdownMode = EAsyncOperationCloseMode::Cancel;
	};

	// Reports execution, result-handle, and deferred callable evidence for one group.
	struct FAsyncOperationGroupSnapshot
	{
		FName OwnerName;
		FName GroupName;
		uint64 OwnerGeneration = 0;
		uint64 GroupId = 0;
		EAsyncOperationGroupState State = EAsyncOperationGroupState::Invalid;
		EAsyncOperationAbortReason AbortReason = EAsyncOperationAbortReason::None;
		uint64 AcceptedCount = 0;
		uint64 RejectedCount = 0;
		uint64 ActiveTaskCount = 0;
		uint64 RetainedResultCount = 0;
		uint32 RetainedDeferredCallableCount = 0;
		uint64 RetainedDeferredCallableBytes = 0;
		bool bWorkerCallablesRetained = false;
	};

	// Aggregates every operation group owned by one module load generation.
	struct FAsyncOperationOwnerSnapshot
	{
		FName OwnerName;
		uint64 OwnerGeneration = 0;
		uint32 GroupCount = 0;
		uint64 ActiveTaskCount = 0;
		uint64 RetainedResultCount = 0;
		uint32 RetainedDeferredCallableCount = 0;
		uint64 RetainedDeferredCallableBytes = 0;
		uint32 GroupsWithWorkerCallables = 0;
		std::vector<FAsyncOperationGroupSnapshot> Groups;
	};

	// Carries a bounded drain outcome and the evidence that authorized or rejected it.
	struct FAsyncOperationDrainResult
	{
		EAsyncOperationDrainStatus Status = EAsyncOperationDrainStatus::Invalid;
		FAsyncOperationOwnerSnapshot Snapshot;
		std::string Message;

		[[nodiscard]] auto Succeeded() const -> bool { return Status == EAsyncOperationDrainStatus::Succeeded; }
	};

	// Binds task roots and descendants to one module-owned close and drain boundary.
	class FAsyncOperationGroup final
	{
	public:
		FAsyncOperationGroup() = default;
		CORE_API ~FAsyncOperationGroup();
		FAsyncOperationGroup(const FAsyncOperationGroup&) = delete;
		auto operator=(const FAsyncOperationGroup&) -> FAsyncOperationGroup& = delete;
		CORE_API FAsyncOperationGroup(FAsyncOperationGroup&& Other) noexcept;
		CORE_API auto operator=(FAsyncOperationGroup&& Other) noexcept -> FAsyncOperationGroup&;

		[[nodiscard]] CORE_API auto IsValid() const -> bool;
		CORE_API auto GetTaskScope() -> FTaskScopeToken;
		CORE_API auto GetCancellationToken() const -> FTaskCancellationToken;
		CORE_API auto Close(
			EAsyncOperationCloseMode Mode,
			EAsyncOperationAbortReason Reason = EAsyncOperationAbortReason::OwnerRequested
		) -> EAsyncOperationCloseStatus;
		CORE_API auto Drain(std::chrono::milliseconds Timeout = std::chrono::seconds(5)) -> FAsyncOperationDrainResult;
		CORE_API auto GetSnapshot() const -> FAsyncOperationGroupSnapshot;

	private:
		explicit FAsyncOperationGroup(std::shared_ptr<Detail::FAsyncOperationGroupState> InState);
		std::shared_ptr<Detail::FAsyncOperationGroupState> State;

		friend class FModuleContext;
		friend struct Detail::FAsyncOperationGroupState;
	};

	namespace Detail
	{
		CORE_API auto CreateAsyncOperationGroup(
			const std::shared_ptr<FModularFeatureOwnerState>& Owner,
			FName GroupName,
			FAsyncOperationGroupOptions Options
		) -> FAsyncOperationGroup;
		CORE_API auto BeginRetireAsyncOperationOwner(
			const std::shared_ptr<FModularFeatureOwnerState>& Owner
		) -> FAsyncOperationOwnerSnapshot;
		CORE_API auto DrainAsyncOperationOwner(
			const std::shared_ptr<FModularFeatureOwnerState>& Owner,
			std::chrono::milliseconds Timeout
		) -> FAsyncOperationDrainResult;
		CORE_API auto SnapshotAsyncOperationOwner(
			const std::shared_ptr<FModularFeatureOwnerState>& Owner
		) -> FAsyncOperationOwnerSnapshot;
	}
}

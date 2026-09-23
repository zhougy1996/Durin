#include "Modules/AsyncOperationGroup.h"

#include "CoreGlobals.h"
#include "Modules/ModuleOwnerState.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextAsyncOperationGroupId = 1;

		auto MapScopeState(ETaskScopeState State) -> EAsyncOperationGroupState
		{
			switch (State)
			{
			case ETaskScopeState::Open: return EAsyncOperationGroupState::Open;
			case ETaskScopeState::ClosingDrain: return EAsyncOperationGroupState::ClosingDrain;
			case ETaskScopeState::ClosingCancel: return EAsyncOperationGroupState::ClosingCancel;
			case ETaskScopeState::QuiescentDrain: return EAsyncOperationGroupState::QuiescentDrain;
			case ETaskScopeState::QuiescentCancel: return EAsyncOperationGroupState::QuiescentCancel;
			default: return EAsyncOperationGroupState::Invalid;
			}
		}
	}

	namespace Detail
	{
		// Owns one task scope and explicit cancellation source independently of Plugin handles.
		struct FAsyncOperationGroupState : std::enable_shared_from_this<FAsyncOperationGroupState>
		{
			FAsyncOperationGroupState(
				std::shared_ptr<FModuleOwnerState> InOwner,
				FName InName,
				FTaskScope InScope)
				: OwnerName(InOwner->Name)
				, OwnerGeneration(InOwner->Generation)
				, Name(InName)
				, Scope(std::move(InScope))
				, GroupId(GNextAsyncOperationGroupId.fetch_add(1, std::memory_order_acq_rel))
			{
			}

			auto MakeHandle() -> FAsyncOperationGroup
			{
				return FAsyncOperationGroup(shared_from_this());
			}

			FName OwnerName;
			uint64 OwnerGeneration = 0;
			FName Name;
			FTaskScope Scope;
			FTaskCancellationSource Cancellation;
			uint64 GroupId = 0;
			mutable std::mutex Mutex;
			EAsyncOperationGroupState State = EAsyncOperationGroupState::Open;
			EAsyncOperationAbortReason AbortReason = EAsyncOperationAbortReason::None;
		};

		auto CloseGroup(
			const std::shared_ptr<FAsyncOperationGroupState>& Group,
			EAsyncOperationCloseMode Mode,
			EAsyncOperationAbortReason Reason
		) -> EAsyncOperationCloseStatus
		{
			if (!Group) return EAsyncOperationCloseStatus::Invalid;
			std::lock_guard Lock(Group->Mutex);
			const ETaskScopeCloseResult ScopeResult = Group->Scope.Close(
				Mode == EAsyncOperationCloseMode::Cancel ? ETaskScopeCloseMode::Cancel : ETaskScopeCloseMode::Drain);
			if (Mode == EAsyncOperationCloseMode::Cancel)
			{
				Group->Cancellation.RequestCancellation();
				if (Group->AbortReason == EAsyncOperationAbortReason::None) Group->AbortReason = Reason;
			}
			Group->State = MapScopeState(Group->Scope.GetDiagnostics().State);
			switch (ScopeResult)
			{
			case ETaskScopeCloseResult::Closed: return EAsyncOperationCloseStatus::Closed;
			case ETaskScopeCloseResult::EscalatedToCancel: return EAsyncOperationCloseStatus::EscalatedToCancel;
			case ETaskScopeCloseResult::AlreadyClosed: return EAsyncOperationCloseStatus::AlreadyClosed;
			default: return EAsyncOperationCloseStatus::Invalid;
			}
		}

		auto SnapshotGroup(const std::shared_ptr<FAsyncOperationGroupState>& Group) -> FAsyncOperationGroupSnapshot
		{
			FAsyncOperationGroupSnapshot Result;
			if (!Group) return Result;
			FTaskScopeToken ScopeToken;
			{
				std::lock_guard Lock(Group->Mutex);
				Result.OwnerName = Group->OwnerName;
				Result.OwnerGeneration = Group->OwnerGeneration;
				Result.GroupName = Group->Name;
				Result.GroupId = Group->GroupId;
				Result.AbortReason = Group->AbortReason;
				ScopeToken = Group->Scope.GetToken();
			}
			const FTaskScopeDiagnostics Scope = Group->Scope.GetDiagnostics();
			Result.State = MapScopeState(Scope.State);
			Result.AcceptedCount = Scope.AcceptedCount;
			Result.RejectedCount = Scope.RejectedCount;
			Result.ActiveTaskCount = Scope.CurrentActiveCount;
			Result.RetainedResultCount = Scope.CurrentRetainedResultCount;
			const Private::FTaskScopeDeferredWorkSnapshot Deferred =
				Private::GetGameThreadDeferredScopeSnapshot(ScopeToken);
			Result.RetainedDeferredCallableCount = Deferred.RetainedCallableCount;
			Result.bWorkerCallablesRetained = Private::GetTaskScopeWorkerCallableCount(ScopeToken) != 0;
			return Result;
		}

		auto DrainGroup(
			const std::shared_ptr<FAsyncOperationGroupState>& Group,
			std::chrono::milliseconds Timeout
		) -> EAsyncOperationDrainStatus
		{
			if (!Group)
			{
				return EAsyncOperationDrainStatus::Invalid;
			}
			FTaskScopeToken ScopeToken;
			bool bCancel = false;
			bool bOpen = false;
			{
				std::lock_guard Lock(Group->Mutex);
				bOpen = Group->State == EAsyncOperationGroupState::Open;
				ScopeToken = Group->Scope.GetToken();
				bCancel = Group->State == EAsyncOperationGroupState::ClosingCancel
					|| Group->State == EAsyncOperationGroupState::QuiescentCancel;
			}
			if (bOpen)
			{
				return EAsyncOperationDrainStatus::Open;
			}
			if (Private::IsExecutingTaskScope(ScopeToken))
			{
				return EAsyncOperationDrainStatus::SelfWait;
			}

			const auto Start = std::chrono::steady_clock::now();
			const auto Deadline = Start + std::max(std::chrono::milliseconds(0), Timeout);
			for (;;)
			{
				const bool bOnGameThread = GIsGameThreadIdInitialized && IsInGameThread();
				if (bOnGameThread)
				{
					const auto Pump = Private::ProcessGameThreadDeferredScope(
						ScopeToken, bCancel, {.bUnlimited = true});
					if (Pump.bReentrant)
					{
						return EAsyncOperationDrainStatus::UnsupportedThread;
					}
				}
				else if (Private::GetGameThreadDeferredScopeSnapshot(ScopeToken).RetainedCallableCount != 0)
				{
					return EAsyncOperationDrainStatus::UnsupportedThread;
				}

				const FAsyncOperationGroupSnapshot Snapshot = SnapshotGroup(Group);
				if (Snapshot.ActiveTaskCount == 0
					&& Snapshot.RetainedResultCount == 0
					&& Snapshot.RetainedDeferredCallableCount == 0
					&& !Snapshot.bWorkerCallablesRetained)
				{
					{
						std::lock_guard Lock(Group->Mutex);
						Group->State = bCancel
							? EAsyncOperationGroupState::QuiescentCancel
							: EAsyncOperationGroupState::QuiescentDrain;
					}
					return EAsyncOperationDrainStatus::Succeeded;
				}

				const auto Now = std::chrono::steady_clock::now();
				if (Now >= Deadline)
				{
					return EAsyncOperationDrainStatus::TimedOut;
				}
				const double RemainingSeconds = std::chrono::duration<double>(Deadline - Now).count();
				if (bOnGameThread)
				{
					std::this_thread::yield();
				}
				else
				{
					const ETaskScopeWaitResult Wait = Group->Scope.WaitFor(std::min(0.001, RemainingSeconds));
					if (Wait == ETaskScopeWaitResult::UnsupportedThread)
					{
						return EAsyncOperationDrainStatus::UnsupportedThread;
					}
				}
				(void)Private::WaitForTaskScopeWorkerCallables(ScopeToken, 0.0);
			}
		}
	}

	FAsyncOperationGroup::~FAsyncOperationGroup() = default;

	FAsyncOperationGroup::FAsyncOperationGroup(std::shared_ptr<Detail::FAsyncOperationGroupState> InState)
		: State(std::move(InState))
	{
	}

	FAsyncOperationGroup::FAsyncOperationGroup(FAsyncOperationGroup&& Other) noexcept = default;
	auto FAsyncOperationGroup::operator=(FAsyncOperationGroup&& Other) noexcept -> FAsyncOperationGroup& = default;
	auto FAsyncOperationGroup::IsValid() const -> bool { return State != nullptr; }
	auto FAsyncOperationGroup::GetTaskScope() -> FTaskScopeToken { return State ? State->Scope.GetToken() : FTaskScopeToken{}; }
	auto FAsyncOperationGroup::GetCancellationToken() const -> FTaskCancellationToken
	{
		return State ? State->Cancellation.GetToken() : FTaskCancellationToken{};
	}
	auto FAsyncOperationGroup::Close(EAsyncOperationCloseMode Mode, EAsyncOperationAbortReason Reason)
		-> EAsyncOperationCloseStatus
	{
		return Detail::CloseGroup(State, Mode, Reason);
	}
	auto FAsyncOperationGroup::Drain(std::chrono::milliseconds Timeout) -> EAsyncOperationDrainStatus
	{
		return Detail::DrainGroup(State, Timeout);
	}
	auto FAsyncOperationGroup::GetSnapshot() const -> FAsyncOperationGroupSnapshot
	{
		return Detail::SnapshotGroup(State);
	}

	auto Detail::CreateAsyncOperationGroup(
		const std::shared_ptr<FModuleOwnerState>& Owner,
		FName GroupName
	) -> FAsyncOperationGroup
	{
		if (!Owner || GroupName.IsNone()) return {};
		FTaskScope Scope = CreateTaskScope();
		if (!Scope.IsValid()) return {};
		auto Group = std::make_shared<FAsyncOperationGroupState>(Owner, GroupName, std::move(Scope));
		return Group->MakeHandle();
	}
}

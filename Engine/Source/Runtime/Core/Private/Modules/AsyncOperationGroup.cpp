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

		auto MakeOwnerSnapshot(FAsyncOperationGroupSnapshot Group) -> FAsyncOperationOwnerSnapshot
		{
			FAsyncOperationOwnerSnapshot Result;
			Result.OwnerName = Group.OwnerName;
			Result.OwnerGeneration = Group.OwnerGeneration;
			Result.GroupCount = 1;
			Result.ActiveTaskCount = Group.ActiveTaskCount;
			Result.RetainedResultCount = Group.RetainedResultCount;
			Result.RetainedDeferredCallableCount = Group.RetainedDeferredCallableCount;
			Result.RetainedDeferredCallableBytes = Group.RetainedDeferredCallableBytes;
			Result.GroupsWithWorkerCallables = Group.bWorkerCallablesRetained ? 1u : 0u;
			Result.Groups.emplace_back(std::move(Group));
			return Result;
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
				FAsyncOperationGroupOptions InOptions,
				FTaskScope InScope)
				: Owner(InOwner)
				, OwnerName(InOwner->Name)
				, OwnerGeneration(InOwner->Generation)
				, Name(InName)
				, Options(InOptions)
				, Scope(std::move(InScope))
				, GroupId(GNextAsyncOperationGroupId.fetch_add(1, std::memory_order_acq_rel))
			{
			}

			auto MakeHandle() -> FAsyncOperationGroup
			{
				return FAsyncOperationGroup(shared_from_this());
			}

			std::weak_ptr<FModuleOwnerState> Owner;
			FName OwnerName;
			uint64 OwnerGeneration = 0;
			FName Name;
			FAsyncOperationGroupOptions Options;
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
			Result.RetainedDeferredCallableBytes = Deferred.RetainedCallableBytes;
			Result.bWorkerCallablesRetained = Private::GetTaskScopeWorkerCallableCount(ScopeToken) != 0;
			return Result;
		}

		auto DrainGroup(
			const std::shared_ptr<FAsyncOperationGroupState>& Group,
			std::chrono::milliseconds Timeout
		) -> FAsyncOperationDrainResult
		{
			if (!Group)
			{
				return {EAsyncOperationDrainStatus::Invalid, {}, "The operation group is invalid."};
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
				return {EAsyncOperationDrainStatus::Open, MakeOwnerSnapshot(SnapshotGroup(Group)),
					"Operation-group admission is still open."};
			}
			if (Private::IsExecutingTaskScope(ScopeToken))
			{
				return {EAsyncOperationDrainStatus::SelfWait, MakeOwnerSnapshot(SnapshotGroup(Group)),
					"Operation-group drain cannot wait from one of its own tasks."};
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
						return {EAsyncOperationDrainStatus::UnsupportedThread, MakeOwnerSnapshot(SnapshotGroup(Group)),
							"Selected Game Thread drain cannot reenter an active deferred-work pump."};
					}
				}
				else if (Private::GetGameThreadDeferredScopeSnapshot(ScopeToken).RetainedCallableCount != 0)
				{
					return {EAsyncOperationDrainStatus::UnsupportedThread, MakeOwnerSnapshot(SnapshotGroup(Group)),
						"GameThreadDeferred work requires drain on the Game Thread."};
				}

				FAsyncOperationGroupSnapshot Snapshot = SnapshotGroup(Group);
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
					Snapshot.State = bCancel ? EAsyncOperationGroupState::QuiescentCancel : EAsyncOperationGroupState::QuiescentDrain;
					return {EAsyncOperationDrainStatus::Succeeded, MakeOwnerSnapshot(std::move(Snapshot)),
						"Operation group is quiescent and retains no callable or result storage."};
				}

				const auto Now = std::chrono::steady_clock::now();
				if (Now >= Deadline)
				{
					return {EAsyncOperationDrainStatus::TimedOut, MakeOwnerSnapshot(std::move(Snapshot)),
						"Timed out waiting for operation execution and retained storage."};
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
						return {EAsyncOperationDrainStatus::UnsupportedThread, MakeOwnerSnapshot(SnapshotGroup(Group)),
							"The current thread cannot wait for this operation group's task scope."};
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
	auto FAsyncOperationGroup::Drain(std::chrono::milliseconds Timeout) -> FAsyncOperationDrainResult
	{
		return Detail::DrainGroup(State, Timeout);
	}
	auto FAsyncOperationGroup::GetSnapshot() const -> FAsyncOperationGroupSnapshot
	{
		return Detail::SnapshotGroup(State);
	}

	auto Detail::CreateAsyncOperationGroup(
		const std::shared_ptr<FModuleOwnerState>& Owner,
		FName GroupName,
		FAsyncOperationGroupOptions Options
	) -> FAsyncOperationGroup
	{
		if (!Owner || GroupName.IsNone() || Owner->bOperationAdmissionRetired.load(std::memory_order_acquire)) return {};
		FTaskScope Scope = CreateTaskScope();
		if (!Scope.IsValid()) return {};
		auto Group = std::make_shared<FAsyncOperationGroupState>(Owner, GroupName, Options, std::move(Scope));
		{
			std::lock_guard Lock(Owner->OperationMutex);
			if (Owner->bOperationAdmissionRetired.load(std::memory_order_acquire)) return {};
			Owner->OperationGroups.emplace_back(Group);
		}
		return Group->MakeHandle();
	}

	auto Detail::BeginRetireAsyncOperationOwner(
		const std::shared_ptr<FModuleOwnerState>& Owner
	) -> FAsyncOperationOwnerSnapshot
	{
		if (!Owner) return {};
		Owner->bOperationAdmissionRetired.store(true, std::memory_order_release);
		std::vector<std::shared_ptr<FAsyncOperationGroupState>> Groups;
		{
			std::lock_guard Lock(Owner->OperationMutex);
			Groups = Owner->OperationGroups;
		}
		for (const auto& Group : Groups)
		{
			CloseGroup(Group, Group->Options.ShutdownMode, EAsyncOperationAbortReason::ModuleShutdown);
		}
		return SnapshotAsyncOperationOwner(Owner);
	}

	auto Detail::DrainAsyncOperationOwner(
		const std::shared_ptr<FModuleOwnerState>& Owner,
		std::chrono::milliseconds Timeout
	) -> FAsyncOperationDrainResult
	{
		if (!Owner) return {EAsyncOperationDrainStatus::Invalid, {}, "The module operation owner is invalid."};
		std::vector<std::shared_ptr<FAsyncOperationGroupState>> Groups;
		{
			std::lock_guard Lock(Owner->OperationMutex);
			Groups = Owner->OperationGroups;
		}
		const auto Deadline = std::chrono::steady_clock::now() + std::max(std::chrono::milliseconds(0), Timeout);
		for (const auto& Group : Groups)
		{
			const auto Now = std::chrono::steady_clock::now();
			const auto Remaining = Now >= Deadline
				? std::chrono::milliseconds(0)
				: std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Now);
			const FAsyncOperationDrainResult Result = DrainGroup(Group, Remaining);
			if (!Result.Succeeded())
			{
				return {Result.Status, SnapshotAsyncOperationOwner(Owner), Result.Message};
			}
		}
		return {EAsyncOperationDrainStatus::Succeeded, SnapshotAsyncOperationOwner(Owner),
			"All module-owned operation groups are quiescent."};
	}

	auto Detail::SnapshotAsyncOperationOwner(
		const std::shared_ptr<FModuleOwnerState>& Owner
	) -> FAsyncOperationOwnerSnapshot
	{
		FAsyncOperationOwnerSnapshot Result;
		if (!Owner) return Result;
		Result.OwnerName = Owner->Name;
		Result.OwnerGeneration = Owner->Generation;
		std::vector<std::shared_ptr<FAsyncOperationGroupState>> Groups;
		{
			std::lock_guard Lock(Owner->OperationMutex);
			Groups = Owner->OperationGroups;
		}
		Result.GroupCount = static_cast<uint32>(Groups.size());
		Result.Groups.reserve(Groups.size());
		for (const auto& Group : Groups)
		{
			auto Snapshot = SnapshotGroup(Group);
			Result.ActiveTaskCount += Snapshot.ActiveTaskCount;
			Result.RetainedResultCount += Snapshot.RetainedResultCount;
			Result.RetainedDeferredCallableCount += Snapshot.RetainedDeferredCallableCount;
			Result.RetainedDeferredCallableBytes += Snapshot.RetainedDeferredCallableBytes;
			Result.GroupsWithWorkerCallables += Snapshot.bWorkerCallablesRetained ? 1u : 0u;
			Result.Groups.emplace_back(std::move(Snapshot));
		}
		return Result;
	}
}

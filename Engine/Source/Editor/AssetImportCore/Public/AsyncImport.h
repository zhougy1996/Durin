#pragma once

#include "AssetImportCore.h"
#include "Threading/Task.h"

namespace Durin::Asset
{
	struct FAsyncImportRequestState;
	struct FAsyncImportExecutionState;
	struct FImportJobOperationState;
	class FAsyncImportCoordinator;
	class FImportJobRegistry;
	class FImportOperationHandle;

	enum class EAsyncImportPlanStatus : uint8
	{
		Invalid,
		Pending,
		Succeeded,
		Failed,
		Canceled,
		Superseded,
		Rejected
	};

	enum class EImportOperationState : uint8
	{
		Queued,
		Running,
		Canceling,
		Finalizing,
		Succeeded,
		Failed,
		Canceled,
		Superseded,
		Rejected,

		// Stage 7 removes this compatibility spelling with the legacy plan API.
		Pending = Running
	};

	inline constexpr size_t MaximumAsyncImportProgressHistory = 32;
	inline constexpr size_t MaximumRetainedImportOperations = 256;
	inline constexpr size_t MaximumImportOutcomeIdentities = 4'096;
	inline constexpr uint64 MaximumImportJobDetachedValueBytes = 1ull << 30;

	enum class EImportOperationLifetime : uint8
	{
		EphemeralPreview,
		EditorOperation,
		SessionCritical
	};

	struct FImportOperationOwner
	{
		std::string OwnerId;
		std::vector<std::string> ConflictIdentities;

		auto operator==(const FImportOperationOwner&) const -> bool = default;
	};

	struct FImportOutcome
	{
		EImportOperationState State = EImportOperationState::Failed;
		std::vector<FImportDiagnostic> Diagnostics;
		std::vector<std::string> PublishedAssetIdentities;
		std::vector<std::string> OrphanAssetIdentities;
		std::optional<std::string> RevealIdentity;
		std::string Diagnostic;

		auto IsTerminal() const -> bool
		{
			return State == EImportOperationState::Succeeded
				|| State == EImportOperationState::Failed
				|| State == EImportOperationState::Canceled
				|| State == EImportOperationState::Superseded
				|| State == EImportOperationState::Rejected;
		}
		auto operator==(const FImportOutcome&) const -> bool = default;
	};

	// The operation transition table is shared by the scheduled and inline job
	// runners. Re-entering a state and leaving a terminal state are invalid.
	ASSETIMPORTCORE_API auto IsImportOperationTransitionAllowed(
		EImportOperationState From, EImportOperationState To) -> bool;

	// Immutable-by-copy presentation state. Worker tasks only publish value data;
	// widgets observe copies through the handle and are never retained by imports.
	struct FImportOperationSnapshot
	{
		uint64 OperationId = 0;
		uint64 Revision = 0;
		std::string OwnerId;
		std::string ProviderId;
		std::string Title;
		EImportPhase Phase = EImportPhase::Snapshot;
		EImportOperationState State = EImportOperationState::Queued;
		std::string SourceIdentity;
		std::string OutputIdentity;
		uint64 CompletedWork = 0;
		uint64 TotalWork = 0;
		std::optional<float> Progress;
		bool bCancelable = true;
		bool bRunningInBackground = false;
		std::string Diagnostic;

		auto IsTerminal() const -> bool
		{
			return State == EImportOperationState::Succeeded
				|| State == EImportOperationState::Failed
				|| State == EImportOperationState::Canceled
				|| State == EImportOperationState::Superseded
				|| State == EImportOperationState::Rejected;
		}

		auto operator==(const FImportOperationSnapshot&) const -> bool = default;
	};

	// Copyable observation handle for one complete service-owned import job.
	// Dropping this handle never owns or cancels accepted EditorOperation or
	// SessionCritical work.
	class ASSETIMPORTCORE_API FImportOperationHandle
	{
	public:
		FImportOperationHandle() = default;

		auto IsValid() const -> bool { return LegacyState != nullptr || JobState != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetOperationId() const -> uint64;
		auto GetSnapshot() const -> FImportOperationSnapshot;
		auto GetProgressHistory() const -> std::vector<FImportOperationSnapshot>;
		auto TryGetOutcome(FImportOutcome& OutOutcome) const -> bool;
		auto RequestCancel() const -> bool;
		auto SetRunningInBackground(bool bRunningInBackground = true) const -> bool;

	private:
		explicit FImportOperationHandle(
			std::shared_ptr<FAsyncImportRequestState> InState)
			: LegacyState(std::move(InState)) {}
		explicit FImportOperationHandle(
			std::shared_ptr<FImportJobOperationState> InState)
			: JobState(std::move(InState)) {}

		std::shared_ptr<FAsyncImportRequestState> LegacyState;
		std::shared_ptr<FImportJobOperationState> JobState;

		friend class FAsyncImportPlanHandle;
		friend class FImportService;
		friend class FAsyncImportCoordinator;
		friend class FImportJobRegistry;
	};

	// Copyable observation handle. The coordinator owns accepted work; retaining
	// a handle does not retain a provider lease after its result is consumed.
	class ASSETIMPORTCORE_API FAsyncImportPlanHandle
	{
	public:
		FAsyncImportPlanHandle() = default;

		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetSerial() const -> uint64;
		auto GetStatus() const -> EAsyncImportPlanStatus;
		// Stage 7 removes this adapter after all production callers accept the
		// operation handle directly.
		auto GetOperationHandle() const -> FImportOperationHandle;
		auto GetOperationSnapshot() const -> FImportOperationSnapshot;
		auto GetProgressHistory() const -> std::vector<FImportOperationSnapshot>;
		auto SetRunningInBackground(bool bRunningInBackground = true) const -> bool;
		auto CreateProgressReporter() const -> std::shared_ptr<IImportProgressReporter>;
		// Extended execution tasks launched after plan consumption must join this
		// scope so provider/owner shutdown can cancel and drain the whole operation.
		auto GetOperationTaskScope() const -> FTaskScopeToken;
		auto IsCancellationRequested() const -> bool;
		auto CompleteOperation(
			EImportOperationState TerminalState, std::string_view Diagnostic = {}) const -> bool;

	private:
		explicit FAsyncImportPlanHandle(
			std::shared_ptr<FAsyncImportRequestState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<FAsyncImportRequestState> State;

		friend class FImportService;
		friend ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
			const FAsyncImportPlanHandle&, FImportPlanResult&) -> EAsyncImportPlanStatus;
		friend class FAsyncImportCoordinator;
		friend class FImportOperationHandle;
	};

	// Drains value-only completion notices on the editor thread. Results and
	// provider leases remain in request state until taken or discarded.
	ASSETIMPORTCORE_API auto DrainAsyncImportCompletionMailbox() -> uint64;
	ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
		const FAsyncImportPlanHandle& Handle,
		FImportPlanResult& OutResult) -> EAsyncImportPlanStatus;

	// Synchronous provider code can poll this at bounded CPU phase boundaries.
	// It returns false outside a coordinator-owned worker task.
	ASSETIMPORTCORE_API auto IsImportCancellationRequested() -> bool;
	ASSETIMPORTCORE_API auto IsImportWorkerPreparation() -> bool;
	class ASSETIMPORTCORE_API FScopedImportWorkerPreparation
	{
	public:
		FScopedImportWorkerPreparation();
		~FScopedImportWorkerPreparation();
		FScopedImportWorkerPreparation(const FScopedImportWorkerPreparation&) = delete;
		auto operator=(const FScopedImportWorkerPreparation&)
			-> FScopedImportWorkerPreparation& = delete;
	};
	ASSETIMPORTCORE_API auto CheckImportEditorMutationAllowed(
		std::string_view Operation) -> void;

	// Provider-owned detached values cross the worker/editor boundary through
	// this small shell. AssetImportCore owns scheduling, cancellation, and task
	// scope mechanics; providers retain the typed value and publication logic.
	struct FDetachedImportBuildResult
	{
		bool bSucceeded = false;
		bool bCanceled = false;
		std::string Message;
		std::shared_ptr<void> Value;
	};

	struct FAsyncImportExecutionRequest
	{
		FTaskScopeToken OperationScope;
		FTaskAttribution Attribution;
		uint64 EstimatedResultBytes = 64ull * 1'024ull;
		std::function<FDetachedImportBuildResult(const FTaskCancellationToken&)> Build;
	};

	class ASSETIMPORTCORE_API FAsyncImportExecutionHandle
	{
	public:
		FAsyncImportExecutionHandle() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }

	private:
		explicit FAsyncImportExecutionHandle(
			std::shared_ptr<FAsyncImportExecutionState> InState)
			: State(std::move(InState)) {}
		std::shared_ptr<FAsyncImportExecutionState> State;

		friend ASSETIMPORTCORE_API auto LaunchAsyncImportExecution(
			FAsyncImportExecutionRequest) -> FAsyncImportExecutionHandle;
		friend ASSETIMPORTCORE_API auto PollAsyncImportExecution(
			FAsyncImportExecutionHandle&, FDetachedImportBuildResult&)
			-> EAsyncImportPlanStatus;
		friend ASSETIMPORTCORE_API auto CancelAndDrainAsyncImportExecution(
			FAsyncImportExecutionHandle&) -> void;
	};

	ASSETIMPORTCORE_API auto LaunchAsyncImportExecution(
		FAsyncImportExecutionRequest Request) -> FAsyncImportExecutionHandle;
	ASSETIMPORTCORE_API auto PollAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle,
		FDetachedImportBuildResult& OutResult) -> EAsyncImportPlanStatus;
	ASSETIMPORTCORE_API auto CancelAndDrainAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle) -> void;
}

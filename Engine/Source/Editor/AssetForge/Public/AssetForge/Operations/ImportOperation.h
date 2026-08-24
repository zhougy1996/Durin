#pragma once

#include "AssetForge/ImportTypes.h"
#include "Threading/Task.h"

namespace Durin::AssetForge
{
	struct FAsyncImportExecutionState;
	struct FImportJobOperationState;
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
	ASSETFORGE_API auto IsImportOperationTransitionAllowed(
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
	class ASSETFORGE_API FImportOperationHandle
	{
	public:
		FImportOperationHandle() = default;

		auto IsValid() const -> bool { return JobState != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetOperationId() const -> uint64;
		auto GetSnapshot() const -> FImportOperationSnapshot;
		auto GetProgressHistory() const -> std::vector<FImportOperationSnapshot>;
		auto TryGetOutcome(FImportOutcome& OutOutcome) const -> bool;
		auto RequestCancel() const -> bool;
		auto SetRunningInBackground(bool bRunningInBackground = true) const -> bool;

	private:
		explicit FImportOperationHandle(
			std::shared_ptr<FImportJobOperationState> InState)
			: JobState(std::move(InState)) {}

		std::shared_ptr<FImportJobOperationState> JobState;

		friend class FImportService;
		friend class FImportJobRegistry;
	};

	// Synchronous provider code can poll this at bounded CPU phase boundaries.
	// It returns false outside a coordinator-owned worker task.
	ASSETFORGE_API auto IsImportCancellationRequested() -> bool;
	ASSETFORGE_API auto IsImportWorkerPreparation() -> bool;
	class ASSETFORGE_API FScopedImportWorkerPreparation
	{
	public:
		FScopedImportWorkerPreparation();
		~FScopedImportWorkerPreparation();
		FScopedImportWorkerPreparation(const FScopedImportWorkerPreparation&) = delete;
		auto operator=(const FScopedImportWorkerPreparation&)
			-> FScopedImportWorkerPreparation& = delete;
	};
	ASSETFORGE_API auto CheckImportEditorMutationAllowed(
		std::string_view Operation) -> void;

	// Provider-owned detached values cross the worker/editor boundary through
	// this small shell. AssetForge owns scheduling, cancellation, and task
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

	class ASSETFORGE_API FAsyncImportExecutionHandle
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

		friend ASSETFORGE_API auto LaunchAsyncImportExecution(
			FAsyncImportExecutionRequest) -> FAsyncImportExecutionHandle;
		friend ASSETFORGE_API auto PollAsyncImportExecution(
			FAsyncImportExecutionHandle&, FDetachedImportBuildResult&)
			-> EAsyncImportPlanStatus;
		friend ASSETFORGE_API auto CancelAndDrainAsyncImportExecution(
			FAsyncImportExecutionHandle&) -> void;
	};

	ASSETFORGE_API auto LaunchAsyncImportExecution(
		FAsyncImportExecutionRequest Request) -> FAsyncImportExecutionHandle;
	ASSETFORGE_API auto PollAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle,
		FDetachedImportBuildResult& OutResult) -> EAsyncImportPlanStatus;
	ASSETFORGE_API auto CancelAndDrainAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle) -> void;
}

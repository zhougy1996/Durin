#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DPackage;
	class FReferenceCollector;
	struct FMaterialError;
	struct FPropertySnapshotError;
}

namespace Durin::Editor
{
	struct FTransactionSnapshotError;
	using FTransactionId = uint64;
	using FRevisionId = uint64;

	// Identifies the history transition reported by a transaction event.
	enum class ETransactionEventType : uint8
	{
		Executed,
		Undone,
		Redone,
		Failed,
		Discarded,
		Evicted,
	};

	// Identifies the transaction operation that produced an event or failure.
	enum class ETransactionOperation : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	struct FTransactionRecordError;
	struct FTransactorResult;
	enum class ETransactionCompletionError : uint8 { None, Operation, Finalization };
	struct FTransactionCompletionError
	{
		ETransactionCompletionError Code = ETransactionCompletionError::None;
		FTransactionId TransactionId = 0;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		std::shared_ptr<const FTransactionRecordError> RecordCause;
		std::shared_ptr<const FTransactorResult> FinalizationCause;
	};
	struct FTransactionCompletionResult
	{
		FTransactionCompletionError Error;
		explicit operator bool() const { return Error.Code == ETransactionCompletionError::None; }
	};
	using FTransactionDeferredCompletion = std::function<void(FTransactionCompletionResult)>;
	DURINED_API auto FormatTransactionCompletionError(const FTransactionCompletionError& Error) -> std::string;

	// Carries one user-visible transaction history event.
	struct FTransactionEvent
	{
		ETransactionEventType Type = ETransactionEventType::Executed;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		FTransactionId Id = 0;
		std::string Description;
		std::string Details;
	};

	enum class ETransactionCustomError : uint8 { None, TargetUnavailable, MaterialWrite, PresentationWrite, MemberUnavailable, MembershipChanged, PropertyRestore, TransformWrite, EmptySelection, ParentMismatch, ParentInvalid, ParentCycle, AttachmentWrite, ResourceUnavailable, ActorMembership, ActorNameCollision, ActorSpawn, ActorDestroy, WorldEnding, ActorType, ActorUnsupported, ActorChanged, ActorRename, InjectedMutation, RollbackIncomplete };
	enum class ETransactionMutationPhase : uint8 { None, TemporaryRename, Remove, Create, FinalRename, Update };
	enum class ETransactionActorConstraint : uint8 { None, Class, ComponentGraph, Parent, Children, BeginningPlay, EndingPlay };
	struct FTransactionCustomError
	{
		ETransactionCustomError Code = ETransactionCustomError::None;
		FGuid ParameterId;
		std::string TargetPath;
		std::string TargetLabel;
		std::string ExpectedParentPath, ActualParentPath;
		size_t NodeCount = 0;
		size_t MemberIndex = 0;
		ETransactionMutationPhase MutationPhase = ETransactionMutationPhase::None;
		ETransactionActorConstraint ActorConstraint = ETransactionActorConstraint::None;
		std::shared_ptr<const FTransactionCustomError> CleanupCause;
		std::shared_ptr<const FTransactionSnapshotError> MemberCause;
		std::shared_ptr<const FPropertySnapshotError> PropertyCause;
		std::shared_ptr<const FMaterialError> MaterialCause;
	};
	struct FTransactionCustomResult
	{
		FTransactionCustomError Error;
		explicit operator bool() const { return Error.Code == ETransactionCustomError::None; }
	};
	DURINED_API auto FormatTransactionCustomError(const FTransactionCustomError& Error) -> std::string;

	// Defines a reversible editor operation stored in transaction history.
	class ITransactionCustomChange
	{
	public:
		virtual ~ITransactionCustomChange() = default;
		DURINED_API virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(ETransactionOperation Operation) const -> std::string { (void)Operation; return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		// True only when a successful transition changes files or discovery
		// identities beneath automatically scanned mounted content.
		virtual auto MutatesMountedContent() const -> bool { return false; }
		virtual auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult = 0;
		virtual auto IsDeferredOperationPending() const -> bool { return false; }
		virtual auto SetDeferredOperationCompletion(
			FTransactionDeferredCompletion Completion) -> void { (void)Completion; }
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) const -> void
		{
			(void)Collector;
		}
		// Reports native allocations in addition to the interface object itself.
		virtual auto GetAllocatedSize() const -> size_t { return 0; }
		// Module-owned executable changes must be drained before this module retires.
		virtual auto GetOwningModule() const -> std::string_view { return {}; }
	};

	// Describes one package's editor-session revision state.
	struct FPackageRevisionState
	{
		FRevisionId CurrentRevision = 0;
		FRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};
}

#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DPackage;
}

namespace Durin::Editor
{
	using FTransactionId = uint64;
	using FRevisionId = uint64;
	using FTransactionDeferredCompletion = std::function<void(bool)>;

	// Identifies the history transition reported by a transaction event.
	enum class ETransactionEventType : uint8
	{
		Executed,
		Undone,
		Redone,
		Failed,
	};

	// Identifies the transaction operation that produced an event or failure.
	enum class ETransactionOperation : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	// Carries one user-visible transaction history event.
	struct FTransactionEvent
	{
		ETransactionEventType Type = ETransactionEventType::Executed;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		FTransactionId Id = 0;
		std::string Description;
		std::string Details;
	};

	// Defines a reversible editor operation stored in transaction history.
	class ITransaction
	{
	public:
		virtual ~ITransaction() = default;
		DURINED_API virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(ETransactionOperation Operation) const -> std::string { (void)Operation; return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		// True only when a successful transition changes files or discovery
		// identities beneath automatically scanned mounted content.
		virtual auto MutatesMountedContent() const -> bool { return false; }
		DURINED_API virtual auto Undo() -> bool = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
		virtual auto IsDeferredOperationPending() const -> bool { return false; }
		virtual auto SetDeferredOperationCompletion(
			FTransactionDeferredCompletion Completion) -> void { (void)Completion; }
	};

	// Describes one package's editor-session revision state.
	struct FPackageRevisionState
	{
		FRevisionId CurrentRevision = 0;
		FRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};

	// Owns bounded undo/redo stacks and emits their user-visible outcomes.
	class FTransactionManager
	{
	public:
		DURINED_API FTransactionManager();
		DURINED_API ~FTransactionManager();
		FTransactionManager(const FTransactionManager&) = delete;
		auto operator=(const FTransactionManager&) -> FTransactionManager& = delete;
		DURINED_API auto Execute(std::unique_ptr<ITransaction> Transaction) -> bool;
		DURINED_API auto CommitApplied(std::unique_ptr<ITransaction> Transaction) -> bool;
		DURINED_API auto Undo() -> bool;
		DURINED_API auto Undo(FTransactionId ExpectedId) -> bool;
		DURINED_API auto Redo() -> bool;
		DURINED_API auto Redo(FTransactionId ExpectedId) -> bool;
		auto CanUndo() const -> bool { return PendingTransactionId == 0 && !UndoStack.empty(); }
		auto CanRedo() const -> bool { return PendingTransactionId == 0 && !RedoStack.empty(); }
		auto HasPendingOperation() const -> bool { return PendingTransactionId != 0; }
		DURINED_API auto IsUndoHead(FTransactionId Id) const -> bool;
		DURINED_API auto IsRedoHead(FTransactionId Id) const -> bool;
		DURINED_API auto GetUndoDescription() const -> std::string_view;
		DURINED_API auto GetRedoDescription() const -> std::string_view;
		DURINED_API auto GetUndoId() const -> FTransactionId;
		DURINED_API auto GetRedoId() const -> FTransactionId;
		auto GetMountedContentMutationRevision() const -> uint64
		{
			return MountedContentMutationRevision;
		}
		DURINED_API auto NotifyMountedContentMutation() -> void;
		DURINED_API auto ConsumeEvents() -> std::vector<FTransactionEvent>;
		DURINED_API auto EstablishSavedState(DPackage& Package) -> void;
		DURINED_API auto MarkSaved(DPackage& Package) -> void;
		DURINED_API auto InvalidateSavedState(DPackage& Package) -> void;
		DURINED_API auto GetPackageRevisionState(const DPackage& Package) const -> std::optional<FPackageRevisionState>;
		DURINED_API auto ForgetPackage(DPackage& Package) -> void;
		DURINED_API auto Clear() -> void;

	private:
		struct FPackageRevisionTransition
		{
			DPackage* Package = nullptr;
			FRevisionId BeforeRevision = 0;
			FRevisionId AfterRevision = 0;
			FRevisionId InitialSavedRevision = 0;
			bool bInitialCheckpointValid = false;
		};

		// Couples a monotonic session identifier with its reversible operation.
		struct FEntry
		{
			FTransactionId Id = 0;
			std::unique_ptr<ITransaction> Transaction;
			std::vector<FPackageRevisionTransition> PackageTransitions;
		};

		struct FTrackedPackageState;

		auto PrepareEntry(std::unique_ptr<ITransaction> Transaction) -> FEntry;
		auto AllocateRevision() -> FRevisionId;
		auto FindPackageState(const DPackage& Package) -> FTrackedPackageState*;
		auto FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*;
		auto EnsurePackageState(DPackage& Package) -> FTrackedPackageState&;
		auto ApplyPackageTransitions(const FEntry& Entry, bool bForward) -> void;
		auto FinalizeUndo(FTransactionId Id) -> void;
		auto FinalizeRedo(FTransactionId Id) -> void;
		auto CompleteDeferredOperation(
			ETransactionOperation Operation,
			FTransactionId Id,
			bool bSucceeded) -> void;
		auto SynchronizeDirtyState(FTrackedPackageState& State) -> void;
		auto RemovePackageHistory(const DPackage& Package) -> void;
		auto RecordFailure(ETransactionOperation Operation, FTransactionId Id, std::string_view Description, std::string_view Details) -> void;

		static constexpr size_t MaxHistory = 256;
		FTransactionId NextId = 1;
		FRevisionId NextRevision = 1;
		uint64 MountedContentMutationRevision = 1;
		ETransactionOperation PendingOperation = ETransactionOperation::Execute;
		FTransactionId PendingTransactionId = 0;
		std::vector<FEntry> UndoStack;
		std::vector<FEntry> RedoStack;
		std::vector<FTransactionEvent> PendingEvents;
		std::unordered_map<DPackage*, std::unique_ptr<FTrackedPackageState>> PackageStates;
	};
}

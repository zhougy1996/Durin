#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	using FEditorTransactionId = uint64;
	using FEditorRevisionId = uint64;

	class DPackage;

	// Identifies the history transition reported by a transaction event.
	enum class EEditorTransactionEventType : uint8
	{
		Executed,
		Undone,
		Redone,
		Failed,
	};

	// Identifies the transaction operation that produced an event or failure.
	enum class EEditorTransactionOperation : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	// Carries one user-visible transaction history event.
	struct FEditorTransactionEvent
	{
		EEditorTransactionEventType Type = EEditorTransactionEventType::Executed;
		EEditorTransactionOperation Operation = EEditorTransactionOperation::Execute;
		FEditorTransactionId Id = 0;
		std::string Description;
		std::string Details;
	};

	// Defines a reversible editor operation stored in transaction history.
	class IEditorTransaction
	{
	public:
		virtual ~IEditorTransaction() = default;
		DURINED_API virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(EEditorTransactionOperation Operation) const -> std::string { (void)Operation; return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		DURINED_API virtual auto Undo() -> bool = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
	};

	// Describes one package's editor-session revision state.
	struct FEditorPackageRevisionState
	{
		FEditorRevisionId CurrentRevision = 0;
		FEditorRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};

	// Owns bounded undo/redo stacks and emits their user-visible outcomes.
	class FEditorTransactionManager
	{
	public:
		DURINED_API FEditorTransactionManager();
		DURINED_API ~FEditorTransactionManager();
		FEditorTransactionManager(const FEditorTransactionManager&) = delete;
		auto operator=(const FEditorTransactionManager&) -> FEditorTransactionManager& = delete;
		DURINED_API auto Execute(std::unique_ptr<IEditorTransaction> Transaction) -> bool;
		DURINED_API auto CommitApplied(std::unique_ptr<IEditorTransaction> Transaction) -> bool;
		DURINED_API auto Undo() -> bool;
		DURINED_API auto Undo(FEditorTransactionId ExpectedId) -> bool;
		DURINED_API auto Redo() -> bool;
		DURINED_API auto Redo(FEditorTransactionId ExpectedId) -> bool;
		auto CanUndo() const -> bool { return !UndoStack.empty(); }
		auto CanRedo() const -> bool { return !RedoStack.empty(); }
		DURINED_API auto IsUndoHead(FEditorTransactionId Id) const -> bool;
		DURINED_API auto IsRedoHead(FEditorTransactionId Id) const -> bool;
		DURINED_API auto GetUndoDescription() const -> std::string_view;
		DURINED_API auto GetRedoDescription() const -> std::string_view;
		DURINED_API auto GetUndoId() const -> FEditorTransactionId;
		DURINED_API auto GetRedoId() const -> FEditorTransactionId;
		DURINED_API auto ConsumeEvents() -> std::vector<FEditorTransactionEvent>;
		DURINED_API auto EstablishSavedState(DPackage& Package) -> void;
		DURINED_API auto MarkSaved(DPackage& Package) -> void;
		DURINED_API auto InvalidateSavedState(DPackage& Package) -> void;
		DURINED_API auto GetPackageRevisionState(const DPackage& Package) const -> std::optional<FEditorPackageRevisionState>;
		DURINED_API auto ForgetPackage(DPackage& Package) -> void;
		DURINED_API auto Clear() -> void;

	private:
		struct FPackageRevisionTransition
		{
			DPackage* Package = nullptr;
			FEditorRevisionId BeforeRevision = 0;
			FEditorRevisionId AfterRevision = 0;
			FEditorRevisionId InitialSavedRevision = 0;
			bool bInitialCheckpointValid = false;
		};

		// Couples a monotonic session identifier with its reversible operation.
		struct FEntry
		{
			FEditorTransactionId Id = 0;
			std::unique_ptr<IEditorTransaction> Transaction;
			std::vector<FPackageRevisionTransition> PackageTransitions;
		};

		struct FTrackedPackageState;

		auto PrepareEntry(std::unique_ptr<IEditorTransaction> Transaction) -> FEntry;
		auto AllocateRevision() -> FEditorRevisionId;
		auto FindPackageState(const DPackage& Package) -> FTrackedPackageState*;
		auto FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*;
		auto EnsurePackageState(DPackage& Package) -> FTrackedPackageState&;
		auto ApplyPackageTransitions(const FEntry& Entry, bool bForward) -> void;
		auto SynchronizeDirtyState(FTrackedPackageState& State) -> void;
		auto RemovePackageHistory(const DPackage& Package) -> void;
		auto RecordFailure(EEditorTransactionOperation Operation, FEditorTransactionId Id, std::string_view Description, std::string_view Details) -> void;

		static constexpr size_t MaxHistory = 256;
		FEditorTransactionId NextId = 1;
		FEditorRevisionId NextRevision = 1;
		std::vector<FEntry> UndoStack;
		std::vector<FEntry> RedoStack;
		std::vector<FEditorTransactionEvent> PendingEvents;
		std::unordered_map<DPackage*, std::unique_ptr<FTrackedPackageState>> PackageStates;
	};
}

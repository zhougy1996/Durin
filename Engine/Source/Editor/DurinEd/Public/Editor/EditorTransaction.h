#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	using FEditorTransactionId = uint64;

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
		DURINED_API virtual auto Undo() -> bool = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
	};

	// Owns bounded undo/redo stacks and emits their user-visible outcomes.
	class FEditorTransactionManager
	{
	public:
		FEditorTransactionManager() = default;
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
		DURINED_API auto Clear() -> void;

	private:
		// Couples a monotonic session identifier with its reversible operation.
		struct FEntry
		{
			FEditorTransactionId Id = 0;
			std::unique_ptr<IEditorTransaction> Transaction;
		};

		auto RecordFailure(EEditorTransactionOperation Operation, FEditorTransactionId Id, std::string_view Description, std::string_view Details) -> void;

		static constexpr size_t MaxHistory = 256;
		FEditorTransactionId NextId = 1;
		std::vector<FEntry> UndoStack;
		std::vector<FEntry> RedoStack;
		std::vector<FEditorTransactionEvent> PendingEvents;
	};
}

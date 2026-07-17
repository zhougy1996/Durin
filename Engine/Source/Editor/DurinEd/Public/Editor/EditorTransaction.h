#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	using FEditorTransactionId = uint64;

	enum class EEditorTransactionEventType : uint8
	{
		Executed,
		Undone,
		Redone,
		Failed,
	};

	enum class EEditorTransactionOperation : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	struct FEditorTransactionEvent
	{
		EEditorTransactionEventType Type = EEditorTransactionEventType::Executed;
		EEditorTransactionOperation Operation = EEditorTransactionOperation::Execute;
		FEditorTransactionId Id = 0;
		std::string Description;
		std::string Details;
	};

	class DURINED_API IEditorTransaction
	{
	public:
		virtual ~IEditorTransaction() = default;
		virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(EEditorTransactionOperation Operation) const -> std::string { (void)Operation; return {}; }
		virtual auto Undo() -> bool = 0;
		virtual auto Redo() -> bool = 0;
	};

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

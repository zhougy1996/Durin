#include "Editor/EditorTransaction.h"

namespace Durin
{
	auto FEditorTransactionManager::Execute(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction) return false;
		const std::string Description(Transaction->GetDescription());
		const std::string Details(Transaction->GetDetails(EEditorTransactionOperation::Execute));
		if (!Transaction->Redo())
		{
			RecordFailure(EEditorTransactionOperation::Execute, 0, Description, Details);
			return false;
		}
		return CommitApplied(std::move(Transaction));
	}

	auto FEditorTransactionManager::CommitApplied(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction) return false;
		const FEditorTransactionId Id = NextId++;
		const std::string Description(Transaction->GetDescription());
		const std::string Details(Transaction->GetDetails(EEditorTransactionOperation::Execute));
		RedoStack.clear();
		UndoStack.push_back({Id, std::move(Transaction)});
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		PendingEvents.push_back({EEditorTransactionEventType::Executed, EEditorTransactionOperation::Execute, Id, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::Undo() -> bool
	{
		return !UndoStack.empty() && Undo(UndoStack.back().Id);
	}

	auto FEditorTransactionManager::Undo(FEditorTransactionId ExpectedId) -> bool
	{
		if (!IsUndoHead(ExpectedId)) return false;
		FEntry& Entry = UndoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(EEditorTransactionOperation::Undo));
		if (!Entry.Transaction->Undo())
		{
			RecordFailure(EEditorTransactionOperation::Undo, Entry.Id, Description, Details);
			return false;
		}
		FEntry Applied = std::move(Entry);
		UndoStack.pop_back();
		RedoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({EEditorTransactionEventType::Undone, EEditorTransactionOperation::Undo, ExpectedId, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::Redo() -> bool
	{
		return !RedoStack.empty() && Redo(RedoStack.back().Id);
	}

	auto FEditorTransactionManager::Redo(FEditorTransactionId ExpectedId) -> bool
	{
		if (!IsRedoHead(ExpectedId)) return false;
		FEntry& Entry = RedoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(EEditorTransactionOperation::Redo));
		if (!Entry.Transaction->Redo())
		{
			RecordFailure(EEditorTransactionOperation::Redo, Entry.Id, Description, Details);
			return false;
		}
		FEntry Applied = std::move(Entry);
		RedoStack.pop_back();
		UndoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({EEditorTransactionEventType::Redone, EEditorTransactionOperation::Redo, ExpectedId, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::IsUndoHead(FEditorTransactionId Id) const -> bool
	{
		return Id != 0 && !UndoStack.empty() && UndoStack.back().Id == Id;
	}

	auto FEditorTransactionManager::IsRedoHead(FEditorTransactionId Id) const -> bool
	{
		return Id != 0 && !RedoStack.empty() && RedoStack.back().Id == Id;
	}

	auto FEditorTransactionManager::GetUndoDescription() const -> std::string_view
	{
		return UndoStack.empty() ? std::string_view{} : UndoStack.back().Transaction->GetDescription();
	}

	auto FEditorTransactionManager::GetRedoDescription() const -> std::string_view
	{
		return RedoStack.empty() ? std::string_view{} : RedoStack.back().Transaction->GetDescription();
	}

	auto FEditorTransactionManager::GetUndoId() const -> FEditorTransactionId
	{
		return UndoStack.empty() ? 0 : UndoStack.back().Id;
	}

	auto FEditorTransactionManager::GetRedoId() const -> FEditorTransactionId
	{
		return RedoStack.empty() ? 0 : RedoStack.back().Id;
	}

	auto FEditorTransactionManager::ConsumeEvents() -> std::vector<FEditorTransactionEvent>
	{
		std::vector<FEditorTransactionEvent> Events;
		Events.swap(PendingEvents);
		return Events;
	}

	auto FEditorTransactionManager::Clear() -> void
	{
		UndoStack.clear();
		RedoStack.clear();
		PendingEvents.clear();
	}

	auto FEditorTransactionManager::RecordFailure(EEditorTransactionOperation Operation, FEditorTransactionId Id, std::string_view Description, std::string_view Details) -> void
	{
		PendingEvents.push_back({EEditorTransactionEventType::Failed, Operation, Id, std::string(Description), std::string(Details)});
	}
}

#include "Editor/EditorTransaction.h"

namespace Durin
{
	auto FEditorTransactionManager::Execute(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction || !Transaction->Redo()) return false;
		return CommitApplied(std::move(Transaction));
	}

	auto FEditorTransactionManager::CommitApplied(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction) return false;
		RedoStack.clear();
		UndoStack.emplace_back(std::move(Transaction));
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		return true;
	}

	auto FEditorTransactionManager::Undo() -> bool
	{
		if (UndoStack.empty()) return false;
		std::unique_ptr<IEditorTransaction> Transaction = std::move(UndoStack.back());
		UndoStack.pop_back();
		if (!Transaction->Undo()) return false;
		RedoStack.emplace_back(std::move(Transaction));
		return true;
	}

	auto FEditorTransactionManager::Redo() -> bool
	{
		if (RedoStack.empty()) return false;
		std::unique_ptr<IEditorTransaction> Transaction = std::move(RedoStack.back());
		RedoStack.pop_back();
		if (!Transaction->Redo()) return false;
		UndoStack.emplace_back(std::move(Transaction));
		return true;
	}

	auto FEditorTransactionManager::GetUndoDescription() const -> std::string_view
	{
		return UndoStack.empty() ? std::string_view{} : UndoStack.back()->GetDescription();
	}

	auto FEditorTransactionManager::GetRedoDescription() const -> std::string_view
	{
		return RedoStack.empty() ? std::string_view{} : RedoStack.back()->GetDescription();
	}

	auto FEditorTransactionManager::Clear() -> void
	{
		UndoStack.clear();
		RedoStack.clear();
	}
}

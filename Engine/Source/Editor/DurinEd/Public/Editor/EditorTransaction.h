#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DURINED_API IEditorTransaction
	{
	public:
		virtual ~IEditorTransaction() = default;
		virtual auto GetDescription() const -> std::string_view = 0;
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
		DURINED_API auto Redo() -> bool;
		auto CanUndo() const -> bool { return !UndoStack.empty(); }
		auto CanRedo() const -> bool { return !RedoStack.empty(); }
		DURINED_API auto GetUndoDescription() const -> std::string_view;
		DURINED_API auto GetRedoDescription() const -> std::string_view;
		DURINED_API auto Clear() -> void;

	private:
		static constexpr size_t MaxHistory = 256;
		std::vector<std::unique_ptr<IEditorTransaction>> UndoStack;
		std::vector<std::unique_ptr<IEditorTransaction>> RedoStack;
	};
}

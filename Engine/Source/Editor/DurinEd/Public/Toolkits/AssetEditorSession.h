#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DEditorEngine;
	class DObject;

	// Shared persistence and transaction operations for an editing Toolkit.
	class FAssetEditorSession
	{
	public:
		DURINED_API auto BindEditor(DEditorEngine* Editor) -> void;
		DURINED_API auto GetEditor() const -> DEditorEngine*;
		DURINED_API auto Activate(DObject* Object) -> bool;
		DURINED_API auto IsDirty(const DObject* Object) const -> bool;
		DURINED_API auto CanSave(const DObject* Object) const -> bool;
		DURINED_API auto Save(DObject* Object, const std::function<bool()>& BeforeSave,
			const std::function<void(std::string)>& ReportError) -> bool;
		DURINED_API auto Discard(DObject* Object, const std::function<void()>& BeforeDiscard = {},
			const std::function<void(std::string)>& ReportError = {}) -> bool;
		DURINED_API auto CanUndo() const -> bool;
		DURINED_API auto CanRedo() const -> bool;
		DURINED_API auto GetUndoDescription() const -> std::string;
		DURINED_API auto GetRedoDescription() const -> std::string;
		DURINED_API auto Undo() -> bool;
		DURINED_API auto Redo() -> bool;
	private:
		TWeakObjectPtr<DEditorEngine> BoundEditor;
		bool bEditorBound = false;
	};
}

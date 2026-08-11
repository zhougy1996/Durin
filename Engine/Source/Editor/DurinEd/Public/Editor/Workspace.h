#pragma once

#include "DurinEdAPI.h"
#include "Editor/WorkspaceTypes.h"

namespace Durin::Editor
{
	// Defines document, history, menu, and layout services hosted by the editor shell.
	class IWorkspace
	{
	public:
		virtual ~IWorkspace() = default;

		DURINED_API virtual auto GetWorkspaceType() const -> const FWorkspaceTypeId& = 0;
		// Deferred opens keep the current tab metadata until the workspace reports completion.
		DURINED_API virtual auto OpenDocument(const FDocumentTab& Document) -> EDocumentOpenResult = 0;
		DURINED_API virtual auto ActivateDocument(const FDocumentTab& Document) -> void = 0;
		// Called before the manager changes the active document or workspace.
		// Returning false keeps the current host state active.
		virtual auto RequestDeactivate() -> bool { return true; }
		// Release workspace-owned document state only when returning Closed.
		virtual auto RequestCloseDocument(const FDocumentTab& Document) -> EDocumentCloseResult
		{
			return Document.bDirty ? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Closed;
		}
		// Resolve resource-specific persistence before the manager retries a pending close.
		virtual auto SaveDocument(const FDocumentTab& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Resolve resource-specific rollback before the manager retries a pending close.
		virtual auto DiscardDocument(const FDocumentTab& Document) -> bool
		{
			(void)Document;
			return false;
		}
		virtual auto IsDocumentDirty(const FDocumentTab& Document) const -> bool { return Document.bDirty; }
		virtual auto CanSaveActiveDocument() const -> bool { return false; }
		virtual auto SaveActiveDocument() -> bool { return false; }
		virtual auto CanUndo() const -> bool { return false; }
		virtual auto CanRedo() const -> bool { return false; }
		virtual auto GetUndoDescription() const -> std::string_view { return {}; }
		virtual auto GetRedoDescription() const -> std::string_view { return {}; }
		virtual auto Undo() -> bool { return false; }
		virtual auto Redo() -> bool { return false; }
		virtual auto DrawFileMenu() -> void {}
		virtual auto DrawEditMenu() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		DURINED_API virtual auto DrawWorkspace(bool bActive) -> bool = 0;
		DURINED_API virtual auto ResetLayout() -> void = 0;
	};
}

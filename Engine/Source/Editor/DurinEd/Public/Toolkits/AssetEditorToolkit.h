#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Documents/EditorDocumentTypes.h"

namespace Durin { class DPackage; }

namespace Durin::Editor
{
	// Defines document, history, menu, and layout services hosted by the editor shell.
	class IAssetEditorToolkit
	{
	public:
		virtual ~IAssetEditorToolkit() = default;

		DURINED_API virtual auto GetToolkitId() const -> const FAssetEditorToolkitId& = 0;
		// Deferred opens keep the current tab metadata until the toolkit reports completion.
		DURINED_API virtual auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult = 0;
		DURINED_API virtual auto ActivateDocument(const FEditorDocument& Document) -> void = 0;
		// Called before the manager changes the active document or toolkit.
		// Returning false keeps the current host state active.
		virtual auto RequestDeactivate() -> bool { return true; }
		// Release toolkit-owned document state only when returning Closed.
		virtual auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult
		{
			return Document.bDirty ? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Closed;
		}
		// Resolve resource-specific persistence before the manager retries a pending close.
		virtual auto SaveDocument(const FEditorDocument& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Resolve resource-specific rollback before the manager retries a pending close.
		virtual auto DiscardDocument(const FEditorDocument& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Called during package reload publication, before the old graph retires.
		virtual auto OnPackageReloaded(DPackage*, DPackage*) -> void {}
		virtual auto IsDocumentDirty(const FEditorDocument& Document) const -> bool { return Document.bDirty; }
		virtual auto CanSaveActiveDocument() const -> bool { return false; }
		virtual auto SaveActiveDocument() -> bool { return false; }
		virtual auto CanUndo() const -> bool { return false; }
		virtual auto CanRedo() const -> bool { return false; }
		virtual auto GetUndoDescription() const -> std::string { return {}; }
		virtual auto GetRedoDescription() const -> std::string { return {}; }
		virtual auto Undo() -> bool { return false; }
		virtual auto Redo() -> bool { return false; }
		virtual auto DrawFileMenu() -> void {}
		virtual auto DrawEditMenu() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		DURINED_API virtual auto DrawToolkit(bool bActive) -> bool = 0;
		DURINED_API virtual auto ResetLayout() -> void = 0;
	};
}

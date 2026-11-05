#pragma once

#include "CoreMinimal.h"
#include "DObject/DObjectFwd.h"
#include "DurinEdAPI.h"
#include "Framework/TabManager.h"

namespace Durin { class DObject; class DPackage; }

namespace Durin::Editor
{
	// Composes the class-neutral package, focus, active-document, and transaction
	// behavior shared by editable asset workspaces. Concrete editors retain
	// loading, property finalization, previews, builds, and type-specific errors.
	class FEditableAssetDocumentModel
	{
	public:
		DURINED_API auto Activate(const FEditorDocument& Document, DObject* Object) -> bool;
		DURINED_API auto Close(std::string_view ResourceId) -> void;
		auto GetActiveResourceId() const -> std::string_view { return ActiveResourceId; }
		auto GetTabManager() -> FTabManager& { return DocumentHost; }

		DURINED_API auto IsDirty(const DObject* Object) const -> bool;
		DURINED_API auto CanSave(const DObject* Object) const -> bool;
		DURINED_API auto Save(DObject* Object,
			const std::function<bool()>& BeforeSave,
			const std::function<void(std::string)>& ReportError) -> bool;
		// Supply the document's manager to migrate its roots during reload; AfterReload
		// refreshes workspace consumers before the old generation retires.
		DURINED_API auto Discard(DObject* Object,
			const std::function<void()>& BeforeDiscard = {},
			const std::function<void(DPackage*, DPackage*)>& AfterReload = {},
			const std::function<void(std::string)>& ReportError = {},
			FEditorDocumentManager* DocumentManager = nullptr) -> bool;

		DURINED_API auto CanUndo() const -> bool;
		DURINED_API auto CanRedo() const -> bool;
		DURINED_API auto GetUndoDescription() const -> std::string;
		DURINED_API auto GetRedoDescription() const -> std::string;
		DURINED_API auto Undo() -> bool;
		DURINED_API auto Redo() -> bool;

	private:
		FTabManager DocumentHost;
		std::string ActiveResourceId;
	};

	// Composes the root-window, focus, active-document, and immutable command
	// behavior shared by read-only per-resource asset inspectors.
	class FReadOnlyAssetDocumentModel
	{
	public:
		DURINED_API auto Activate(const FEditorDocument& Document, const DObject* Object) -> bool;
		DURINED_API auto Close(std::string_view ResourceId) -> void;
		auto GetActiveResourceId() const -> std::string_view { return ActiveResourceId; }
		auto GetTabManager() -> FTabManager& { return DocumentHost; }

		auto SaveDocument(const FEditorDocument&) const -> bool { return false; }
		auto DiscardDocument(const FEditorDocument&) const -> bool { return false; }
		auto IsDocumentDirty(const FEditorDocument&) const -> bool { return false; }
		auto CanSaveActiveDocument() const -> bool { return false; }
		auto SaveActiveDocument() const -> bool { return false; }

	private:
		FTabManager DocumentHost;
		std::string ActiveResourceId;
	};
}

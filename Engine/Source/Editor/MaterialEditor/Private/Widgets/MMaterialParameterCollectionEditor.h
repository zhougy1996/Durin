#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "PropertyEditor/PropertyView.h"
#include "Workspace/Workspace.h"
#include "Workspace/WorkspaceRootWindow.h"

namespace Durin { class DMaterialParameterCollection; }

namespace Durin::Editor::Material
{
	class MMaterialParameterCollectionEditor final : public IWorkspace
	{
	public:
		explicit MMaterialParameterCollectionEditor(FWorkspaceManager& InManager)
			: Manager(InManager) {}
		auto GetWorkspaceType() const -> const FWorkspaceTypeId& override;
		auto OpenDocument(const FDocumentTab& Document)
			-> EDocumentOpenResult override;
		auto ActivateDocument(const FDocumentTab& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FDocumentTab& Document)
			-> EDocumentCloseResult override;
		auto SaveDocument(const FDocumentTab& Document) -> bool override;
		auto DiscardDocument(const FDocumentTab& Document) -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement)
			-> void override;
		auto IsDocumentDirty(const FDocumentTab& Document) const -> bool override;
		auto CanSaveActiveDocument() const -> bool override;
		auto SaveActiveDocument() -> bool override;
		auto CanUndo() const -> bool override { return Documents.CanUndo(); }
		auto CanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string_view override
			{ return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string_view override
			{ return Documents.GetRedoDescription(); }
		auto Undo() -> bool override { return Documents.Undo(); }
		auto Redo() -> bool override { return Documents.Redo(); }
		auto DrawWorkspace(bool bActive) -> bool override;
		auto ResetLayout() -> void override {}

	private:
		auto Find(std::string_view ResourceId) const
			-> DMaterialParameterCollection*;
		auto Active() const -> DMaterialParameterCollection*;
		auto FinishEdit(bool bCancel) -> bool;
		auto Context() -> FPropertyViewContext;
		auto Report(std::string Message) -> void;

		FWorkspaceManager& Manager;
		std::unordered_map<std::string,
			TObjectPtr<DMaterialParameterCollection>> Collections;
		FEditableAssetDocumentModel Documents;
		FPropertyView PropertyView;
		std::string ErrorMessage;
	};
}

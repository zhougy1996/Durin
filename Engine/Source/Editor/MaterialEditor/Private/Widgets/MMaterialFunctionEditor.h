#pragma once

#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Asset/MutationExtensions.h"
#include "MaterialEditorAPI.h"

namespace Durin::Editor::Material
{
	inline const FWorkspaceTypeId MaterialFunctionWorkspaceType("MaterialFunctionEditor");
	class MMaterialFunctionEditor final : public IWorkspace, public IAssetMoveObserver
	{
	public:
		explicit MMaterialFunctionEditor(FWorkspaceManager& Manager);
		~MMaterialFunctionEditor() override;
		auto GetWorkspaceType() const -> const FWorkspaceTypeId& override { return MaterialFunctionWorkspaceType; }
		auto OpenDocument(const FDocumentTab& Document) -> EDocumentOpenResult override;
		auto ActivateDocument(const FDocumentTab& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FDocumentTab& Document) -> EDocumentCloseResult override;
		auto SaveDocument(const FDocumentTab& Document) -> bool override;
		auto DiscardDocument(const FDocumentTab& Document) -> bool override;
		auto IsDocumentDirty(const FDocumentTab& Document) const -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto CanSaveActiveDocument() const -> bool override;
		auto SaveActiveDocument() -> bool override;
		auto CanUndo() const -> bool override { return Documents.CanUndo(); }
		auto CanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string_view override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string_view override { return Documents.GetRedoDescription(); }
		auto Undo() -> bool override { RequestDeactivate(); return Documents.Undo(); }
		auto Redo() -> bool override { RequestDeactivate(); return Documents.Redo(); }
		auto DrawWorkspace(bool bActive) -> bool override;
		auto ResetLayout() -> void override;
		auto NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool;
		auto GetLastError() const -> std::string_view { return Error; }
	private:
		struct FDocument;
		auto Find(std::string_view Resource) const -> FDocument*;
		auto DrawDocument(const FDocumentTab& Tab, FDocument& Document) -> void;
		auto DrawInterface(FDocument& Document) -> void;
		auto OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void override;
		FWorkspaceManager& Manager;
		FEditableAssetDocumentModel Documents;
		std::unordered_map<std::string, std::unique_ptr<FDocument>> Open;
		FAssetMoveObserverHandle MoveObserver = 0;
		std::string Error;
	};
}

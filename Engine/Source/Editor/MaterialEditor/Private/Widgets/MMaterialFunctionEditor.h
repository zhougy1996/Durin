#pragma once

#include "CoreMinimal.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "Asset/MutationExtensions.h"
#include "MaterialEditorAPI.h"

namespace Durin::Editor::Material
{
	inline const FAssetEditorToolkitId MaterialFunctionWorkspaceType("MaterialFunctionEditor");
	class FMaterialEditorSessionSettings;
	class MMaterialFunctionEditor final : public IAssetEditorToolkit, public IAssetMoveObserver
	{
	public:
		explicit MMaterialFunctionEditor(FEditorDocumentManager& Manager);
		~MMaterialFunctionEditor() override;
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return MaterialFunctionWorkspaceType; }
		auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		auto ActivateDocument(const FEditorDocument& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		auto SaveDocument(const FEditorDocument& Document) -> bool override;
		auto DiscardDocument(const FEditorDocument& Document) -> bool override;
		auto IsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto CanSaveActiveDocument() const -> bool override;
		auto SaveActiveDocument() -> bool override;
		auto CanUndo() const -> bool override { return Documents.CanUndo(); }
		auto CanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Documents.GetRedoDescription(); }
		auto Undo() -> bool override { RequestDeactivate(); return Documents.Undo(); }
		auto Redo() -> bool override { RequestDeactivate(); return Documents.Redo(); }
		auto DrawToolkit(bool bActive) -> bool override;
		auto ResetLayout() -> void override;
		auto NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool;
		auto GetLastError() const -> std::string_view { return Error; }
	private:
		struct FDocument;
		auto Find(std::string_view Resource) const -> FDocument*;
		auto DrawDocument(const FEditorDocument& Tab, FDocument& Document) -> void;
		auto DrawInterface(FDocument& Document) -> void;
		auto DrawInputs(FDocument& Document) -> void;
		auto OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void override;
		std::unique_ptr<FMaterialEditorSessionSettings> SessionSettings;
		FEditorDocumentManager& Manager;
		FEditableAssetDocumentModel Documents;
		std::unordered_map<std::string, std::unique_ptr<FDocument>> Open;
		FAssetMoveObserverHandle MoveObserver = 0;
		std::string Error;
	};
}

#pragma once

#include "CoreMinimal.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "Asset/MutationExtensions.h"
#include "MaterialEditorAPI.h"

namespace Durin::MaterialEditor
{
	inline const FAssetEditorToolkitId MaterialFunctionWorkspaceType("MaterialFunctionEditor");
	class FMaterialEditorSessionSettings;
	class MMaterialFunctionEditor final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return Error; }
		auto GetLayoutVersion() const -> uint32 override;
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		explicit MMaterialFunctionEditor(FEditorDocumentManager& Manager);
		~MMaterialFunctionEditor() override;
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return MaterialFunctionWorkspaceType; }
		auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		auto ActivateDocument(const FEditorDocument& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		auto OnSaveDocument(const FEditorDocument& Document) -> bool override;
		auto OnDiscardDocument(const FEditorDocument& Document) -> bool override;
		auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto OnCanSaveActiveDocument() const -> bool override;
		auto OnSaveActiveDocument() -> bool override;
		auto OnCanUndo() const -> bool override { return Documents.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Documents.GetRedoDescription(); }
		auto OnUndo() -> bool override { RequestDeactivate(); return Documents.Undo(); }
		auto OnRedo() -> bool override { RequestDeactivate(); return Documents.Redo(); }
		auto OnDrawToolkit(bool bActive) -> bool override;
		auto ResetLayout() -> void override;
		auto NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool;
		auto GetLastError() const -> std::string_view { return Error; }
		auto OnHostChanged() -> void override;
	protected:
		auto OnSessionBound() -> void override;
	private:
		struct FDocument;
		auto Find(std::string_view Resource) const -> FDocument*;
		auto DrawDocument(const FEditorDocument& Tab, FDocument& Document) -> void;
		auto DrawInterface(FDocument& Document) -> void;
		auto DrawInputs(FDocument& Document) -> void;
		std::unique_ptr<FMaterialEditorSessionSettings> SessionSettings;
		FEditorDocumentManager& Manager;
		FEditableAssetDocumentModel Documents;
		std::unordered_map<std::string, std::unique_ptr<FDocument>> Open;
		std::string Error;
	};
}

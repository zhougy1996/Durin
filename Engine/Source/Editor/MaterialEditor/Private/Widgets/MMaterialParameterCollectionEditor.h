#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "PropertyEditor/PropertyView.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"

namespace Durin { class DMaterialParameterCollection; }

namespace Durin::Editor::Material
{
	class MMaterialParameterCollectionEditor final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		explicit MMaterialParameterCollectionEditor(FEditorDocumentManager& InManager);
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		auto OpenDocument(const FEditorDocument& Document)
			-> EDocumentOpenResult override;
		auto ActivateDocument(const FEditorDocument& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FEditorDocument& Document)
			-> EDocumentCloseResult override;
		auto OnSaveDocument(const FEditorDocument& Document) -> bool override;
		auto OnDiscardDocument(const FEditorDocument& Document) -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement)
			-> void override;
		auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		auto OnCanSaveActiveDocument() const -> bool override;
		auto OnSaveActiveDocument() -> bool override;
		auto OnCanUndo() const -> bool override { return Documents.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string override
			{ return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override
			{ return Documents.GetRedoDescription(); }
		auto OnUndo() -> bool override { return Documents.Undo(); }
		auto OnRedo() -> bool override { return Documents.Redo(); }
		auto OnDrawToolkit(bool bActive) -> bool override;
		auto ResetLayout() -> void override {}

	private:
		auto Find(std::string_view ResourceId) const
			-> DMaterialParameterCollection*;
		auto Active() const -> DMaterialParameterCollection*;
		auto FinishEdit(bool bCancel) -> bool;
		auto Context() -> FPropertyViewContext;
		auto Report(std::string Message) -> void;

		FEditorDocumentManager& Manager;
		std::unordered_map<std::string,
			TObjectPtr<DMaterialParameterCollection>> Collections;
		FEditableAssetDocumentModel Documents;
		FPropertyView PropertyView;
		std::string ErrorMessage;
	};
}

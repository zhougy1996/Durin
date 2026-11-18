#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "PropertyEditor/PropertyView.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorSession.h"

namespace Durin { class DMaterialParameterCollection; }

namespace Durin::MaterialEditor
{
	class MMaterialParameterCollectionEditor final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		auto OnOpenAsset(DObject& Object) -> bool override;
		auto OnActivateAsset(DObject& Object) -> void override;
		auto RequestDeactivate() -> bool override;
		auto OnClose() -> void override;
		auto OnSaveAsset(DObject& Object) -> bool override;
		auto OnDiscardAsset(DObject& Object) -> bool override;
		auto OnPackageReloaded(DPackage*, DPackage*) -> void override;
		auto OnIsAssetDirty(const DObject& Object) const -> bool override;
		auto OnCanSaveActiveDocument() const -> bool override;
		auto OnCanUndo() const -> bool override { return Session.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Session.CanRedo(); }
		auto GetUndoDescription() const -> std::string override
			{ return Session.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override
			{ return Session.GetRedoDescription(); }
		auto OnUndo() -> bool override { return Session.Undo(); }
		auto OnRedo() -> bool override { return Session.Redo(); }
		auto OnDrawToolkit(bool bActive) -> bool override;
		auto ResetLayout() -> void override {}

	protected:
		auto PostInitAssetEditor() -> void override;
	private:
		auto FinishEdit(bool bCancel) -> bool;
		auto Context() -> FPropertyViewContext;
		auto Report(std::string Message) -> void;

		FAssetEditorSession Session;
		FPropertyView PropertyView;
		std::string ErrorMessage;
	};
}

#pragma once

#include "CoreMinimal.h"
#include "DObject/StrongObjectPtr.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "PropertyEditor/PropertyView.h"
#include "Settings/EditorSettings.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace Durin { class DMaterialInstance; class DMaterialInterface; }

namespace Durin::MaterialEditor
{
	class FMaterialEditorProfileSettings;
	class FMaterialParameterPanelCache;
	class FMaterialParameterPanelModel;
	class FMaterialPreview;
	struct FMaterialParameterPanelEntry;

	class FMaterialInstanceEditor final : public FBaseAssetToolkit
	{
	public:
		explicit FMaterialInstanceEditor(FEditorDocumentManager& InManager);
		~FMaterialInstanceEditor() override;
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		auto GetLayoutVersion() const -> uint32 override;
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		auto ActivateDocument(const FEditorDocument& Document) -> void override;
		auto RequestDeactivate() -> bool override;
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		auto GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>> override;
		auto CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant> override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		auto OnClose() -> void override;
		auto OnHostChanged() -> void override;
		auto ResetLayout() -> void override;
		auto GetUndoDescription() const -> std::string override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Documents.GetRedoDescription(); }

	protected:
		auto PostInitAssetEditor() -> void override;
		auto OnDrawToolkit(bool bActive) -> bool override;
		auto OnSaveDocument(const FEditorDocument&) -> bool override { return OnSaveActiveDocument(); }
		auto OnDiscardDocument(const FEditorDocument&) -> bool override;
		auto OnIsDocumentDirty(const FEditorDocument&) const -> bool override;
		auto OnCanSaveActiveDocument() const -> bool override;
		auto OnSaveActiveDocument() -> bool override;
		auto OnCanUndo() const -> bool override { return Documents.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Documents.CanRedo(); }
		auto OnUndo() -> bool override { return RequestDeactivate() && Documents.Undo(); }
		auto OnRedo() -> bool override { return RequestDeactivate() && Documents.Redo(); }

	private:
		class FReloadParticipant;
		class FMaterialParameterRowScope;
		auto DrawDocument(const FEditorDocument& Document, DMaterialInstance& Instance) -> void;
		auto DrawToolbar(DMaterialInstance& Instance) -> void;
		auto DrawMaterialInstance(DMaterialInstance* Instance) -> void;
		auto DrawParentPicker(DMaterialInstance* Instance) -> void;
		auto DrawMaterialParameters(DMaterialInterface* Material) -> void;
		auto DrawMaterialParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawScalarParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawIntegerParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawVectorParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawColorParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawTextureParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawOrphanParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> FPropertyViewContext;
		auto SetError(std::string Message) -> void;
		auto ReleasePreview() -> void;
		auto GetPreferencesPageId() const -> std::string;

		FEditorDocumentManager& Manager;
		FEditableAssetDocumentModel Documents;
		TStrongObjectPtr<DMaterialInstance> Source;
		FPropertyView PropertyView;
		std::shared_ptr<FMaterialPreview> Preview;
		std::unique_ptr<FMaterialEditorProfileSettings> ProfileSettings;
		std::unique_ptr<FMaterialParameterPanelCache> MaterialParameterPanelCache;
		FEditorSettingsPageRegistration SettingsPage;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::string ErrorMessage;
		bool bPendingLayoutReset = false;
	};
}

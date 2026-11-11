#pragma once

#include "CoreMinimal.h"

#include "Asset/MutationExtensions.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "MaterialEditorAPI.h"
#include "DObject/ObjectPtr.h"
#include "PropertyEditor/PropertyView.h"
#include "Widgets/MaterialFunctionCallPicker.h"
#include "Settings/EditorSettings.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterial;
	class DMaterialInstance;
}

namespace Durin::MaterialEditor
{
	class FMaterialPreview;
	class FMaterialGraphCanvas;
	class FMaterialParameterPanelCache;
	class FMaterialParameterPanelModel;
	class FMaterialEditorSessionSettings;
	class FMaterialEditingSession;
	struct FMaterialParameterPanelEntry;

	// Hosts one material document with preview and parameter editing state.
	class MMaterialEditor final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		auto GetLayoutVersion() const -> uint32 override;
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		explicit MMaterialEditor(
			FEditorDocumentManager& InWorkspaceManager);

		MATERIALEDITOR_API ~MMaterialEditor() override;
		MATERIALEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		MATERIALEDITOR_API auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		MATERIALEDITOR_API auto ActivateDocument(const FEditorDocument& Document) -> void override;
		MATERIALEDITOR_API auto RequestDeactivate() -> bool override;
		MATERIALEDITOR_API auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		MATERIALEDITOR_API auto OnSaveDocument(const FEditorDocument& Document) -> bool override;
		MATERIALEDITOR_API auto OnDiscardDocument(const FEditorDocument& Document) -> bool override;
		MATERIALEDITOR_API auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant> override;
		MATERIALEDITOR_API auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		MATERIALEDITOR_API auto OnCanSaveActiveDocument() const -> bool override;
		MATERIALEDITOR_API auto OnSaveActiveDocument() -> bool override;
		MATERIALEDITOR_API auto OnCanUndo() const -> bool override;
		MATERIALEDITOR_API auto OnCanRedo() const -> bool override;
		MATERIALEDITOR_API auto GetUndoDescription() const -> std::string override;
		MATERIALEDITOR_API auto GetRedoDescription() const -> std::string override;
		MATERIALEDITOR_API auto OnUndo() -> bool override;
		MATERIALEDITOR_API auto OnRedo() -> bool override;
		MATERIALEDITOR_API auto OnDrawToolkit(bool bActive) -> bool override;
		MATERIALEDITOR_API auto ResetLayout() -> void override;
		auto OnHostChanged() -> void override;
		auto ReleaseSessionUI() -> void override;
	protected:
		auto OnSessionBound() -> void override;

	private:
		friend struct FMaterialEditorTestAccess;
		friend class FMaterialEditorReloadParticipant;
		class FMaterialParameterRowScope;

		auto FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*;
		auto FindEditingSession(const DMaterialInterface* Working) const -> FMaterialEditingSession*;
		auto ResetEditingSession(std::string_view ResourceId) -> bool;
		auto GetActiveMaterial() const -> DMaterialInterface*;
		auto SaveMaterial(DMaterialInterface* Material) -> bool;
		auto DrawDocument(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawToolbar(DMaterialInterface* Material) -> void;
		auto DrawPreferences(std::string& OutError) -> void;
		auto GetPreferencesPageId() const -> std::string;
		auto SetAutoCompile(bool bEnabled) -> void;
		auto ResetPreferences() -> bool;
		auto DrawCompileStatus(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawDockLayout(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawPreviewPanel(const FEditorDocument& Document, DMaterialInterface* Material, float Height) -> void;
		auto DrawGraphPanel(const FEditorDocument& Document, DMaterialInterface* Material, float Height) -> void;
		auto DrawDetailsPanel(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawParametersPanel(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawSelectedFunction(const FEditorDocument& Document, DMaterial* Base) -> void;
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
		auto GetOrCreateCanvas(const FEditorDocument& Document)
			-> FMaterialGraphCanvas&;
		auto CancelCanvasInteraction(uint64 DocumentId) -> void;
		auto CaptureCanvasViewport(const FEditorDocument& Document) -> void;

		FEditorDocumentManager& DocumentManager;
		std::unordered_map<std::string, TObjectPtr<DMaterialInterface>> OpenMaterials;
		std::unordered_map<std::string, std::unique_ptr<FMaterialEditingSession>> EditingSessions;
		FEditableAssetDocumentModel Documents;
		std::unordered_map<uint64, std::unique_ptr<FMaterialPreview>> MaterialPreviews;
		std::unordered_map<uint64, std::unique_ptr<FMaterialGraphCanvas>> MaterialGraphCanvases;
		std::unique_ptr<FMaterialParameterPanelCache> MaterialParameterPanelCache;
		std::unique_ptr<FMaterialEditorSessionSettings> SessionSettings;
		FEditorSettingsPageRegistration SettingsPage;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::unordered_map<uint64, std::array<char, 128>> ParameterSearchTexts;
		std::string ErrorMessage;
		FPropertyView PropertyView;
		std::unordered_set<uint64> PendingLayoutResets;
	};
}

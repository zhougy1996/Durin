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
	class DMaterialFunction;
	struct FTransactionEvent;
}

namespace Durin::MaterialEditor
{
	class FMaterialPreview;
	class FMaterialGraphCanvas;
	class FMaterialParameterPanelCache;
	class FMaterialParameterPanelModel;
	class FMaterialEditorProfileSettings;
	class FMaterialEditingSession;
	struct FMaterialParameterPanelEntry;

	// Controls material and function documents with isolated family editing policies.
	enum class EMaterialEditorMode : uint8 { Material, Function };
	inline const FAssetEditorToolkitId MaterialFunctionWorkspaceType("MaterialFunctionEditor");

	class FMaterialEditor final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		auto GetLayoutVersion() const -> uint32 override;
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		explicit FMaterialEditor(
			FEditorDocumentManager& InDocumentManager, EMaterialEditorMode InMode = EMaterialEditorMode::Material);

		MATERIALEDITOR_API ~FMaterialEditor() override;
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
		auto OnClose() -> void override;
		MATERIALEDITOR_API auto NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool;
	protected:
		auto PostInitAssetEditor() -> void override;

	private:
		auto OpenReferencedMaterialAsset(std::string_view Path, std::string_view ClassName) -> bool;
		friend struct FMaterialEditorTestAccess;
		friend struct FMaterialGraphCanvasTestAccess;
		friend struct FMaterialFunctionEditorTestAccess;
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
		auto CancelCanvasInteraction(uint64 DocumentId) -> bool;
		auto CaptureCanvasViewport(const FEditorDocument& Document) -> void;

		struct FFunctionDocument;
		auto ApplyFunctionDocument(std::string_view Resource, bool bForce = false) -> bool;
		auto IsFunctionPublishing(std::string_view Resource) const -> bool;
		auto ReleaseFunctionDocuments() -> void;
		auto RegisterFunctionUndo() -> void;
		auto UnregisterFunctionUndo() -> void;
		auto HandleFunctionTransaction(const FTransactionEvent& Event) -> void;
		auto SaveFunctionViewports() -> void;
		auto FindOpenFunction(std::string_view Resource) const -> DMaterialFunction*;
		auto SaveFunction(DMaterialFunction* Function) -> bool;
		auto DiscardFunctionDocument(const FEditorDocument& Document) -> bool;
		auto IsFunctionDocumentDirty(std::string_view Resource) const -> bool;
		auto ResetFunctionDocument(std::string_view Resource) -> bool;
		auto FindFunctionDocument(std::string_view Resource) const -> FFunctionDocument*;
		auto OpenFunctionDocument(const FEditorDocument& Document) -> EDocumentOpenResult;
		auto CancelFunctionInteractions() -> bool;
		auto CloseFunctionDocument(const FEditorDocument& Document) -> void;
		auto RebindFunctionDocuments(DPackage* Previous, DPackage* Replacement) -> void;
		auto ResetFunctionLayout() -> void;
		auto DrawFunctionDocuments(bool bActive) -> bool;
		auto RemapFunctionDocuments(std::string_view Source, std::string_view Destination) -> void;
		auto DrawDocument(const FEditorDocument& Tab, FFunctionDocument& Document) -> void;
		auto DrawInterface(FFunctionDocument& Document) -> void;
		auto DrawInputs(FFunctionDocument& Document) -> void;
		const EMaterialEditorMode Mode;
		std::unordered_map<std::string, std::shared_ptr<FFunctionDocument>> FunctionDocuments;
		uint64 FunctionUndoHandle = 0;

		FEditorDocumentManager& DocumentManager;
		std::unordered_map<std::string, TObjectPtr<DMaterialInterface>> OpenMaterials;
		std::unordered_map<std::string, std::unique_ptr<FMaterialEditingSession>> EditingSessions;
		FEditableAssetDocumentModel Documents;
		std::unordered_map<uint64, std::shared_ptr<FMaterialPreview>> MaterialPreviews;
		std::unordered_map<uint64, std::unique_ptr<FMaterialGraphCanvas>> MaterialGraphCanvases;
		std::unique_ptr<FMaterialParameterPanelCache> MaterialParameterPanelCache;
		std::unique_ptr<FMaterialEditorProfileSettings> ProfileSettings;
		FEditorSettingsPageRegistration SettingsPage;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::unordered_map<uint64, std::array<char, 128>> ParameterSearchTexts;
		std::string ErrorMessage;
		FPropertyView PropertyView;
		std::unordered_set<uint64> PendingLayoutResets;
	};
}

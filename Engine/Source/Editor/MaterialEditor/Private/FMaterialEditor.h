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
	class FMaterialEditorProfileSettings;
	class FMaterialEditingSession;
	struct FMaterialFunctionEditingSession;

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
		auto GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>> override;
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
		MATERIALEDITOR_API auto NavigateToNode(const FGuid& NodeId) -> bool;
	protected:
		auto PostInitAssetEditor() -> void override;

	private:
		friend struct FMaterialEditorTestAccess;
		friend struct FMaterialGraphCanvasTestAccess;
		friend struct FMaterialFunctionEditorTestAccess;
		friend class FMaterialEditorReloadParticipant;

		auto ResetEditingSession(DMaterial& Source) -> bool;
		auto ReleaseMaterialSession() -> void;
		auto GetActiveMaterial() const -> DMaterialInterface*;
		auto SaveMaterial() -> bool;
		auto DrawDocument(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawToolbar(DMaterialInterface* Material) -> void;
		auto DrawPreferences(std::string& OutError) -> void;
		auto GetPreferencesPageId() const -> std::string;
		auto SetAutoCompile(bool bEnabled) -> void;
		auto ResetPreferences() -> bool;
		auto DrawDockLayout(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawPreviewPanel(DMaterialInterface* Material, float Height) -> void;
		auto DrawGraphPanel(const FEditorDocument& Document, float Height) -> void;
		auto DrawDetailsPanel(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawParametersPanel(const FEditorDocument& Document, DMaterialInterface* Material) -> void;
		auto DrawSelectedFunction(const FEditorDocument& Document, DMaterial* Base) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> FPropertyViewContext;
		auto SetError(std::string Message) -> void;
		auto GetOrCreateCanvas(const FEditorDocument& Document)
			-> FMaterialGraphCanvas&;
		auto CancelCanvasInteraction() -> bool;
		auto CaptureCanvasViewport() -> void;

		auto ApplyFunction(bool bForce = false) -> bool;
		auto SaveFunction() -> bool;
		auto ResetFunctionSession(DMaterialFunction& Source) -> bool;
		auto ReleaseFunctionSession() -> void;
		auto RegisterFunctionUndo() -> void;
		auto UnregisterFunctionUndo() -> void;
		auto HandleFunctionTransaction(const FTransactionEvent& Event) -> void;
		auto SaveFunctionViewport() -> void;
		auto OpenFunctionDocument(const FEditorDocument& Document) -> EDocumentOpenResult;
		auto CancelFunctionInteractions() -> bool;
		auto ResetFunctionLayout() -> void;
		auto DrawFunction(bool bActive) -> bool;
		auto DrawDocument(const FEditorDocument& Tab, FMaterialFunctionEditingSession& Document) -> void;
		auto DrawInterface(FMaterialFunctionEditingSession& Document) -> void;
		auto DrawInputs(FMaterialFunctionEditingSession& Document) -> void;
		const EMaterialEditorMode Mode;
		std::shared_ptr<FMaterialFunctionEditingSession> FunctionSession;
		uint64 FunctionUndoHandle = 0;

		FEditorDocumentManager& DocumentManager;
		std::shared_ptr<FMaterialEditingSession> EditingSession;
		FEditableAssetDocumentModel Documents;
		std::unique_ptr<FMaterialEditorProfileSettings> ProfileSettings;
		FEditorSettingsPageRegistration SettingsPage;
		std::array<char, 128> ParameterSearchText{};
		std::string ErrorMessage;
		FPropertyView PropertyView;
		bool bPendingLayoutReset = false;
	};
}

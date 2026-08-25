#pragma once

#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "MaterialEditorAPI.h"
#include "DObject/ObjectPtr.h"
#include "Editor/PropertyView.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterial;
	class DMaterialInstance;
}

namespace Durin::Editor::Material
{
	class FMaterialPreview;
	class FMaterialGraphCanvas;
	class FMaterialParameterPanelCache;
	class FMaterialParameterPanelModel;
	struct FMaterialParameterPanelEntry;

	// Hosts one material document with preview and parameter editing state.
	class MMaterialEditor final : public ::Durin::Editor::IWorkspace
	{
	public:
		explicit MMaterialEditor(::Durin::Editor::FWorkspaceManager& InWorkspaceManager);
		MATERIALEDITOR_API ~MMaterialEditor() override;
		MATERIALEDITOR_API auto GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId& override;
		MATERIALEDITOR_API auto OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult override;
		MATERIALEDITOR_API auto ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void override;
		MATERIALEDITOR_API auto RequestDeactivate() -> bool override;
		MATERIALEDITOR_API auto RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult override;
		MATERIALEDITOR_API auto SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		MATERIALEDITOR_API auto DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		MATERIALEDITOR_API auto IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool override;
		MATERIALEDITOR_API auto CanSaveActiveDocument() const -> bool override;
		MATERIALEDITOR_API auto SaveActiveDocument() -> bool override;
		MATERIALEDITOR_API auto CanUndo() const -> bool override;
		MATERIALEDITOR_API auto CanRedo() const -> bool override;
		MATERIALEDITOR_API auto GetUndoDescription() const -> std::string_view override;
		MATERIALEDITOR_API auto GetRedoDescription() const -> std::string_view override;
		MATERIALEDITOR_API auto Undo() -> bool override;
		MATERIALEDITOR_API auto Redo() -> bool override;
		MATERIALEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		MATERIALEDITOR_API auto ResetLayout() -> void override;

	private:
		auto FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*;
		auto GetActiveMaterial() const -> DMaterialInterface*;
		auto SaveMaterial(DMaterialInterface* Material) -> bool;
		auto DrawDocument(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void;
		auto DrawToolbar(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void;
		auto DrawCompileStatus(DMaterialInterface* Material) -> void;
		auto DrawWideLayout(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void;
		auto DrawNarrowLayout(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void;
		auto DrawPreviewPanel(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material, float Height) -> void;
		auto DrawGraphPanel(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material, float Height) -> void;
		auto DrawOverviewPanel(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material, float Height) -> void;
		auto DrawDetailsPanel(DMaterialInterface* Material, float Height) -> void;
		auto DrawMaterial(DMaterial* Material) -> void;
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
		auto MakePropertyViewContext() -> ::Durin::Editor::FPropertyViewContext;
		auto SetError(std::string Message) -> void;

		::Durin::Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, TObjectPtr<DMaterialInterface>> OpenMaterials;
		::Durin::Editor::FEditableAssetDocumentModel Documents;
		std::unordered_map<uint64, std::unique_ptr<FMaterialPreview>> MaterialPreviews;
		std::unordered_map<uint64, std::unique_ptr<FMaterialGraphCanvas>> MaterialGraphCanvases;
		std::unique_ptr<FMaterialParameterPanelCache> MaterialParameterPanelCache;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::string ErrorMessage;
		::Durin::Editor::FPropertyView PropertyView;
		float SidebarRatio = 0.40f;
		float PreviewPaneRatio = 0.68f;
		bool bUsePreferredPreviewWidth = true;
	};
}

#pragma once

#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"
#include "MaterialEditorAPI.h"
#include "DObject/ObjectPtr.h"
#include "Editor/ReflectedPropertyView.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterial;
	class DMaterialInstance;
	class FMaterialPreview;
	class FMaterialParameterPanelModel;
	struct FMaterialParameterPanelEntry;

	// Hosts one material document with preview and parameter editing state.
	class MMaterialEditor final : public IEditorWorkspace
	{
	public:
		explicit MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager);
		MATERIALEDITOR_API ~MMaterialEditor() override;
		MATERIALEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		MATERIALEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult override;
		MATERIALEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		MATERIALEDITOR_API auto RequestDeactivate() -> bool override;
		MATERIALEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> bool override;
		MATERIALEDITOR_API auto IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool override;
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
		auto DrawDocument(const FEditorDocumentTab& Document, DMaterialInterface* Material) -> void;
		auto DrawMaterial(DMaterial* Material) -> void;
		auto DrawMaterialInstance(DMaterialInstance* Instance) -> void;
		auto DrawParentPicker(DMaterialInstance* Instance) -> void;
		auto DrawMaterialParameters(DMaterialInterface* Material) -> void;
		auto DrawMaterialParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawScalarParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawColorParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawTextureParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto DrawOrphanParameter(const FMaterialParameterPanelModel& Model, const FMaterialParameterPanelEntry& Entry) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> FReflectedPropertyViewContext;
		auto SetError(std::string Message) -> void;

		FEditorWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, TObjectPtr<DMaterialInterface>> OpenMaterials;
		std::unordered_map<uint64, FEditorWorkspaceRootWindow> DocumentWindows;
		std::unordered_map<uint64, std::unique_ptr<FMaterialPreview>> MaterialPreviews;
		std::string ActiveResourceId;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::string ErrorMessage;
		FReflectedPropertyView PropertyView;
		// Set when a dirty document close is deferred for user confirmation.
		FEditorDocumentId PendingCloseDocumentId;
	};
}

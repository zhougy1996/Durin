#pragma once

#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"
#include "LevelEditorAPI.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterial;
	class DMaterialInstance;

	class MMaterialEditor final : public IEditorWorkspace
	{
	public:
		explicit MMaterialEditor(FEditorWorkspaceManager& InWorkspaceManager);
		LEVELEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		LEVELEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		LEVELEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool override;
		LEVELEDITOR_API auto DrawMainMenu() -> void override;
		LEVELEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	private:
		auto FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*;
		auto GetActiveMaterial() const -> DMaterialInterface*;
		auto SaveMaterial(DMaterialInterface* Material) -> bool;
		auto DrawDocument(const FEditorDocumentTab& Document, DMaterialInterface* Material, bool bActive) -> void;
		auto DrawMaterial(DMaterial* Material) -> void;
		auto DrawMaterialInstance(DMaterialInstance* Instance) -> void;
		auto DrawParentPicker(DMaterialInstance* Instance) -> void;
		auto DrawBaseColor(DMaterialInterface* Material, DMaterialInstance* Instance) -> void;
		auto DrawScalarParameter(DMaterialInterface* Material, DMaterialInstance* Instance, std::string_view Name, const char* Label, float DefaultValue, float Minimum, float Maximum) -> void;
		auto DrawBaseColorTexture(DMaterialInterface* Material, DMaterialInstance* Instance) -> void;
		auto SetError(std::string Message) -> void;

		FEditorWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, TObjectPtr<DMaterialInterface>> OpenMaterials;
		std::unordered_map<uint64, FEditorWorkspaceRootWindow> DocumentWindows;
		std::string ActiveResourceId;
		std::array<char, 128> ParentSearchText{};
		std::array<char, 128> TextureSearchText{};
		std::string ErrorMessage;
	};
}

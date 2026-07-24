#pragma once

#include "MaterialEditorAPI.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FEditorWorkspaceRegistrationHandle;
	class FEditorWorkspaceManager;

	// Registers the material workspace and material asset-editor mapping.
	class FMaterialEditorModule final : public IModuleInterface
	{
	public:
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule() -> void override;
		MATERIALEDITOR_API auto ShutdownModule() -> void override;
		MATERIALEDITOR_API auto RegisterMaterialEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool;
		MATERIALEDITOR_API auto UnregisterMaterialEditorWorkspace() -> void;

	private:
		std::unique_ptr<FEditorWorkspaceRegistrationHandle> WorkspaceRegistration;
	};
}

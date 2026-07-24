#pragma once

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	class FEditorWorkspaceRegistrationHandle;
	class FEditorWorkspaceManager;

	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterTextureEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool;
		TEXTUREEDITOR_API auto UnregisterTextureEditorWorkspace() -> void;

	private:
		std::unique_ptr<FEditorWorkspaceRegistrationHandle> WorkspaceRegistration;
	};
}

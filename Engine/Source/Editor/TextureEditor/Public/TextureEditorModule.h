#pragma once

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	namespace Editor
	{
		class FWorkspaceRegistrationHandle;
		class FWorkspaceManager;
	}
	namespace Editor { class FRenderedAssetThumbnailService; }
	namespace Editor { class FAssetThumbnailProviderRegistrationHandle; }

	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterTextureEditor(
			Editor::FWorkspaceManager& WorkspaceManager,
			Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		TEXTUREEDITOR_API auto UnregisterTextureEditor() -> void;

	private:
		std::unique_ptr<Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::FAssetThumbnailProviderRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<Editor::FAssetThumbnailProviderRegistrationHandle> TextureCubeThumbnailRegistration;
	};
}

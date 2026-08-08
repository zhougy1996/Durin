#pragma once

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	class FEditorWorkspaceRegistrationHandle;
	class FEditorWorkspaceManager;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;

	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterTextureEditor(
			FEditorWorkspaceManager& WorkspaceManager,
			FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		TEXTUREEDITOR_API auto UnregisterTextureEditor() -> void;

	private:
		std::unique_ptr<FEditorWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<FAssetThumbnailProviderRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<FAssetThumbnailProviderRegistrationHandle> TextureCubeThumbnailRegistration;
	};
}

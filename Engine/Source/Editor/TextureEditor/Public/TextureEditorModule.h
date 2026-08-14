#pragma once

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin
{
	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule(FModuleContext& Context) -> void override;
		TEXTUREEDITOR_API auto ShutdownModule(FModuleShutdownContext& Context) -> void override;
		TEXTUREEDITOR_API auto RegisterTextureEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		TEXTUREEDITOR_API auto UnregisterTextureEditor() -> void;

	private:
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> TextureCubeThumbnailRegistration;
	};
}

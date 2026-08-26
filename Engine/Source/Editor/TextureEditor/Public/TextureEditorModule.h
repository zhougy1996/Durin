#pragma once

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FAssetThumbnailProviderRegistry;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin::Editor::Texture { class FTextureImportDialog; }

namespace Durin
{
	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API FTextureEditorModule();
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterTextureEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
			::Durin::Editor::Import::FImportDialogCallbacks ImportCallbacks = {}) -> bool;
		TEXTUREEDITOR_API auto UnregisterTextureEditor() -> void;
		TEXTUREEDITOR_API auto DrawImportDialogs() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> TextureCubeThumbnailRegistration;
		Editor::ContentBrowser::FScopedExtensionRegistration
			ContentBrowserImportExtension;
		Editor::ContentBrowser::FScopedExtensionRegistration
			ContentBrowserReimportExtension;
		std::unique_ptr<Editor::Texture::FTextureImportDialog> ImportDialog;
	};
}

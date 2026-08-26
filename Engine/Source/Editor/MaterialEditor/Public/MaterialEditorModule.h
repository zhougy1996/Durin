#pragma once

#include "MaterialEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FAssetThumbnailProviderRegistry;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin
{
	// Registers the material workspace and material asset-editor mapping.
	class FMaterialEditorModule final : public IModuleInterface
	{
	public:
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule() -> void override;
		MATERIALEDITOR_API auto ShutdownModule() -> void override;
		MATERIALEDITOR_API auto RegisterMaterialEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService) -> bool;
		MATERIALEDITOR_API auto UnregisterMaterialEditor() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> MaterialThumbnailRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> MaterialInstanceThumbnailRegistration;
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};
}

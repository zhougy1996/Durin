#pragma once

#include "MaterialEditorAPI.h"
#include "Modules/ModuleManager.h"

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin
{
	// Registers the material workspace and material asset-editor mapping.
	class FMaterialEditorModule final : public IModuleInterface
	{
	public:
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule(FModuleContext& Context) -> void override;
		MATERIALEDITOR_API auto ShutdownModule(FModuleShutdownContext& Context) -> void override;
		MATERIALEDITOR_API auto RegisterMaterialEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		MATERIALEDITOR_API auto UnregisterMaterialEditor() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> MaterialThumbnailRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> MaterialInstanceThumbnailRegistration;
	};
}

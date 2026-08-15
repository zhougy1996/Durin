#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMeshEditorAPI.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	class FWorkspaceRegistrationHandle;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin
{
	// Registers the read-only StaticMesh inspector and its exact asset route.
	class FStaticMeshEditorModule final : public IModuleInterface
	{
	public:
		STATICMESHEDITOR_API ~FStaticMeshEditorModule() override;
		STATICMESHEDITOR_API auto StartupModule() -> void override;
		STATICMESHEDITOR_API auto ShutdownModule() -> void override;
		STATICMESHEDITOR_API auto RegisterStaticMeshEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		STATICMESHEDITOR_API auto UnregisterStaticMeshEditor() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> ThumbnailRegistration;
	};
}

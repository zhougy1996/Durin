#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMeshEditorAPI.h"

namespace Durin
{
	namespace Editor
	{
		class FWorkspaceManager;
		class FWorkspaceRegistrationHandle;
	}
	namespace Editor { class FRenderedAssetThumbnailService; }
	namespace Editor { class FAssetThumbnailProviderRegistrationHandle; }

	// Registers the read-only StaticMesh inspector and its exact asset route.
	class FStaticMeshEditorModule final : public IModuleInterface
	{
	public:
		STATICMESHEDITOR_API ~FStaticMeshEditorModule() override;
		STATICMESHEDITOR_API auto StartupModule() -> void override;
		STATICMESHEDITOR_API auto ShutdownModule() -> void override;
		STATICMESHEDITOR_API auto RegisterStaticMeshEditor(
			Editor::FWorkspaceManager& WorkspaceManager,
			Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		STATICMESHEDITOR_API auto UnregisterStaticMeshEditor() -> void;

	private:
		std::unique_ptr<Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::FAssetThumbnailProviderRegistrationHandle> ThumbnailRegistration;
	};
}

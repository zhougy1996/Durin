#pragma once

#include "Modules/ModuleManager.h"
#include "SkeletalMeshEditorAPI.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	class FWorkspaceRegistrationHandle;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin
{
	// Registers exact read-only Skeleton, SkeletalMesh, and AnimationClip editor routes.
	class FSkeletalMeshEditorModule final : public IModuleInterface
	{
	public:
		SKELETALMESHEDITOR_API ~FSkeletalMeshEditorModule() override;
		SKELETALMESHEDITOR_API auto StartupModule() -> void override;
		SKELETALMESHEDITOR_API auto ShutdownModule() -> void override;
		SKELETALMESHEDITOR_API auto RegisterSkeletalMeshEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		SKELETALMESHEDITOR_API auto UnregisterSkeletalMeshEditor() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> ThumbnailRegistration;
	};
}

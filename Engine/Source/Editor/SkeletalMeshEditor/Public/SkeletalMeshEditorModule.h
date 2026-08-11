#pragma once

#include "Modules/ModuleManager.h"
#include "SkeletalMeshEditorAPI.h"

namespace Durin
{
	namespace Editor
	{
		class FWorkspaceManager;
		class FWorkspaceRegistrationHandle;
	}
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;

	// Registers exact read-only Skeleton, SkeletalMesh, and AnimationClip editor routes.
	class FSkeletalMeshEditorModule final : public IModuleInterface
	{
	public:
		SKELETALMESHEDITOR_API ~FSkeletalMeshEditorModule() override;
		SKELETALMESHEDITOR_API auto StartupModule() -> void override;
		SKELETALMESHEDITOR_API auto ShutdownModule() -> void override;
		SKELETALMESHEDITOR_API auto RegisterSkeletalMeshEditor(
			Editor::FWorkspaceManager& WorkspaceManager,
			FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		SKELETALMESHEDITOR_API auto UnregisterSkeletalMeshEditor() -> void;

	private:
		std::unique_ptr<Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<FAssetThumbnailProviderRegistrationHandle> ThumbnailRegistration;
	};
}

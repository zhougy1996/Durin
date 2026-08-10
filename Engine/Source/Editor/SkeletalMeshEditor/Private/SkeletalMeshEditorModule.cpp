#include "SkeletalMeshEditorModule.h"

#include "Animation/AnimationClip.h"
#include "Editor/EditorWorkspace.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Widgets/MSkeletalAssetInspector.h"
#include "Workspace/SkeletalMeshEditorWorkspace.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/SkeletalMeshAssetThumbnail.h"

namespace Durin
{
	IMPLEMENT_MODULE(FSkeletalMeshEditorModule, SkeletalMeshEditor)

	FSkeletalMeshEditorModule::~FSkeletalMeshEditorModule() = default;
	auto FSkeletalMeshEditorModule::StartupModule() -> void {}
	auto FSkeletalMeshEditorModule::ShutdownModule() -> void { UnregisterSkeletalMeshEditor(); }

	auto FSkeletalMeshEditorModule::RegisterSkeletalMeshEditor(
		FEditorWorkspaceManager& WorkspaceManager,
		FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		auto Workspace = std::make_shared<MSkeletalAssetInspector>(WorkspaceManager);
		FEditorWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {{
				.Descriptor = {
					.WorkspaceType = SkeletalMeshEditorWorkspace::Type,
					.DisplayName = "Skeletal Asset Inspector",
					.RootKey = std::string(SkeletalMeshEditorWorkspace::RootKey),
					.bShowInWindowMenu = false,
					.bOpenByDefault = false,
					.DefaultHostDockPreference = EEditorWorkspaceHostDockPreference::Center},
				.Workspace = Workspace}},
			.AssetEditors = {
				{.AssetClassName = DSkeleton::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = SkeletalMeshEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource, .bClosable = true},
				{.AssetClassName = DSkeletalMesh::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = SkeletalMeshEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource, .bClosable = true},
				{.AssetClassName = DAnimationClip::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = SkeletalMeshEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource, .bClosable = true}}});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<FEditorWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		FAssetThumbnailProviderRegistrationHandle ThumbnailHandle =
			ThumbnailService.RegisterScoped(
				std::make_unique<FSkeletalMeshAssetThumbnailProvider>(), Error);
		if (!ThumbnailHandle)
		{
			WorkspaceRegistration.reset(); return false;
		}
		ThumbnailRegistration = std::make_unique<FAssetThumbnailProviderRegistrationHandle>(
			std::move(ThumbnailHandle));
		return true;
	}

	auto FSkeletalMeshEditorModule::UnregisterSkeletalMeshEditor() -> void
	{
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}

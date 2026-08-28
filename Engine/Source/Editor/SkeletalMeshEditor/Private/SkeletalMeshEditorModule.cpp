#include "SkeletalMeshEditorModule.h"

#include "Animation/AnimationClip.h"
#include "Editor/WorkspaceManager.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Widgets/MSkeletalAssetInspector.h"
#include "Workspace/SkeletalMeshEditorWorkspace.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/SkeletalMeshThumbnailRenderer.h"

namespace Durin
{
	using namespace Editor::SkeletalMesh;

	IMPLEMENT_MODULE(FSkeletalMeshEditorModule, SkeletalMeshEditor)

	FSkeletalMeshEditorModule::~FSkeletalMeshEditorModule() = default;
	auto FSkeletalMeshEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
	}
	auto FSkeletalMeshEditorModule::ShutdownModule() -> void { UnregisterSkeletalMeshEditor(); }

	auto FSkeletalMeshEditorModule::RegisterSkeletalMeshEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::DThumbnailManager& ThumbnailManager) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		auto Workspace = std::make_shared<MSkeletalAssetInspector>(WorkspaceManager);
		::Durin::Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {{
				.Descriptor = {
					.WorkspaceType = Workspace::Type,
					.DisplayName = "Skeletal Asset Inspector",
					.RootKey = std::string(Workspace::RootKey),
					.bShowInWindowMenu = false,
					.bOpenByDefault = false,
					.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center},
				.Workspace = Workspace}},
			.AssetEditors = {
				{.AssetClassName = DSkeleton::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource, .bClosable = true},
				{.AssetClassName = DSkeletalMesh::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource, .bClosable = true},
				{.AssetClassName = DAnimationClip::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource, .bClosable = true}}},
			EditorExtensionCallbacks.GetGate());
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		::Durin::Editor::FThumbnailRendererRegistrationHandle ThumbnailHandle =
			ThumbnailManager.RegisterScoped(
				std::make_unique<DSkeletalMeshThumbnailRenderer>(),
				EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle)
		{
			WorkspaceRegistration.reset(); return false;
		}
		ThumbnailRegistration = std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
			std::move(ThumbnailHandle));
		return true;
	}

	auto FSkeletalMeshEditorModule::UnregisterSkeletalMeshEditor() -> void
	{
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}

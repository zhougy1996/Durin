#include "StaticMeshEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Widgets/MStaticMeshInspector.h"
#include "Workspace/StaticMeshEditorWorkspace.h"

namespace Durin
{
	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	FStaticMeshEditorModule::~FStaticMeshEditorModule() = default;

	auto FStaticMeshEditorModule::StartupModule() -> void
	{
	}

	auto FStaticMeshEditorModule::ShutdownModule() -> void
	{
		UnregisterStaticMeshEditor();
	}

	auto FStaticMeshEditorModule::RegisterStaticMeshEditor(
		Editor::FWorkspaceManager& WorkspaceManager,
		Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		ThumbnailRegistration.reset();
		std::shared_ptr<MStaticMeshInspector> Workspace = std::make_shared<MStaticMeshInspector>(WorkspaceManager);
		Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = StaticMeshEditorWorkspace::Type,
						.DisplayName = "StaticMesh Inspector",
						.RootKey = std::string(StaticMeshEditorWorkspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DStaticMesh::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = StaticMeshEditorWorkspace::Type,
					.DocumentPolicy = Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<Editor::FWorkspaceRegistrationHandle>(std::move(Registration));

		std::string Error;
		Editor::FAssetThumbnailProviderRegistrationHandle ThumbnailHandle =
			ThumbnailService.RegisterScoped(
				std::make_unique<FStaticMeshAssetThumbnailProvider>(), Error);
		if (!ThumbnailHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		ThumbnailRegistration =
			std::make_unique<Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(ThumbnailHandle));
		return true;
	}

	auto FStaticMeshEditorModule::UnregisterStaticMeshEditor() -> void
	{
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}

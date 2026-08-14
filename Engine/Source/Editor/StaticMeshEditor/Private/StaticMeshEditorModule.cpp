#include "StaticMeshEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Widgets/MStaticMeshInspector.h"
#include "Workspace/StaticMeshEditorWorkspace.h"

namespace Durin
{
	using namespace Editor::StaticMesh;

	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	FStaticMeshEditorModule::~FStaticMeshEditorModule() = default;

	auto FStaticMeshEditorModule::StartupModule(FModuleContext& Context) -> void
	{
		EditorExtensionCallbacks =
			Context.CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
	}

	auto FStaticMeshEditorModule::ShutdownModule(FModuleShutdownContext&) -> void
	{
		UnregisterStaticMeshEditor();
	}

	auto FStaticMeshEditorModule::RegisterStaticMeshEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		ThumbnailRegistration.reset();
		std::shared_ptr<MStaticMeshInspector> Workspace = std::make_shared<MStaticMeshInspector>(WorkspaceManager);
		::Durin::Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = Workspace::Type,
						.DisplayName = "StaticMesh Inspector",
						.RootKey = std::string(Workspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DStaticMesh::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		}, EditorExtensionCallbacks.GetGate());
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));

		std::string Error;
		::Durin::Editor::FAssetThumbnailProviderRegistrationHandle ThumbnailHandle =
			ThumbnailService.RegisterScoped(
				std::make_unique<FStaticMeshAssetThumbnailProvider>(),
				EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		ThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(ThumbnailHandle));
		return true;
	}

	auto FStaticMeshEditorModule::UnregisterStaticMeshEditor() -> void
	{
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}

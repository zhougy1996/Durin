#include "StaticMeshEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Editor/WorkspaceManager.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Widgets/MStaticMeshInspector.h"
#include "Workspace/StaticMeshEditorWorkspace.h"
#include "Import/StaticMeshImportDialog.h"

namespace Durin
{
	using namespace Editor::StaticMesh;
	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	struct FStaticMeshEditorModule::FIntegrationState
	{
		Editor::ContentBrowser::FScopedExtensionRegistration ImportExtension;
		std::unique_ptr<Editor::StaticMesh::FStaticMeshImportDialog> ImportDialog;
	};

	FStaticMeshEditorModule::FStaticMeshEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}
	FStaticMeshEditorModule::~FStaticMeshEditorModule() = default;

	auto FStaticMeshEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
	}

	auto FStaticMeshEditorModule::ShutdownModule() -> void
	{
		UnregisterStaticMeshEditor();
	}

	auto FStaticMeshEditorModule::RegisterStaticMeshEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::DThumbnailManager& ThumbnailManager,
		::Durin::Editor::FImportDialogCallbacks ImportCallbacks) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		ThumbnailRegistration.reset();
		Integration->ImportDialog = std::make_unique<Editor::StaticMesh::FStaticMeshImportDialog>(
			std::move(ImportCallbacks));
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
		if (!Registration)
		{
			UnregisterStaticMeshEditor();
			return false;
		}
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));

		std::string Error;
		::Durin::Editor::FThumbnailRendererRegistrationHandle ThumbnailHandle =
			ThumbnailManager.RegisterScoped(
				std::make_unique<DStaticMeshThumbnailRenderer>(),
				EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle)
		{
			UnregisterStaticMeshEditor();
			return false;
		}
		ThumbnailRegistration =
			std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(ThumbnailHandle));
		auto ImportExtension = Editor::ContentBrowser::RegisterExtension({
			.Id = "static-mesh.import-static-mesh",
			.Label = "Static Mesh (Geometry Only)...",
			.Category = Editor::ContentBrowser::EExtensionCategory::Import,
			.Order = 400,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [this](const auto& Invocation) {
				if (Integration->ImportDialog)
					Integration->ImportDialog->Open(
						Invocation.Context.VirtualDirectory);
			},
			.DrawHostPresentation = [this](bool bAllowAssetMutation) {
				if (Integration->ImportDialog)
					Integration->ImportDialog->Draw(bAllowAssetMutation);
			},
			.OwnerGate = EditorExtensionCallbacks.GetGate(),
		}, Error);
		if (!ImportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser Static Mesh import: {}", Error);
			UnregisterStaticMeshEditor();
			return false;
		}
		Integration->ImportExtension = std::move(ImportExtension);
		return true;
	}

	auto FStaticMeshEditorModule::UnregisterStaticMeshEditor() -> void
	{
		Integration->ImportExtension.Reset();
		Integration->ImportDialog.reset();
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

}

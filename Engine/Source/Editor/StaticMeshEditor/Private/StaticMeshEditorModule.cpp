#include "StaticMeshEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/AssetThumbnailProvider.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Widgets/MStaticMeshInspector.h"
#include "Workspace/StaticMeshEditorWorkspace.h"
#include "Import/StaticMeshImportDialog.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "Dialogs/FileDialog.h"

namespace Durin
{
	using namespace Editor::StaticMesh;
	namespace
	{
		auto ReimportStaticMesh(std::string_view AssetPath,
			std::function<void(std::string)> ReportError) -> void
		{
			auto Report = [&ReportError](std::string Message) {
				if (ReportError) ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(AssetPath, Path))
			{
				Report("The selected StaticMesh path is invalid.");
				return;
			}
			DStaticMesh* Mesh = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Mesh);
			if (!Load || !Mesh)
			{
				Report(Load ? "The selected StaticMesh could not be loaded."
					: Load.Message);
				return;
			}
			std::string Error;
			if (!AssetForge::Builtins::ReimportStaticMesh(
				*Mesh, Error))
			{
				Report(std::move(Error));
				return;
			}
			(void)Asset::UnloadPackage(Path);
		}
	}

	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	struct FStaticMeshEditorModule::FIntegrationState
	{
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
		::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
		::Durin::Editor::Import::FImportDialogCallbacks ImportCallbacks) -> bool
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
		::Durin::Editor::FAssetThumbnailProviderRegistrationHandle ThumbnailHandle =
			ThumbnailService.RegisterScoped(
				std::make_unique<FStaticMeshAssetThumbnailProvider>(),
				EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle)
		{
			UnregisterStaticMeshEditor();
			return false;
		}
		ThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(ThumbnailHandle));
		return true;
	}

	auto FStaticMeshEditorModule::UnregisterStaticMeshEditor() -> void
	{
		Integration->ImportDialog.reset();
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

	auto FStaticMeshEditorModule::OpenImportDialog(std::string_view Directory) -> void
	{
		if (Integration->ImportDialog) Integration->ImportDialog->Open(Directory);
	}

	auto FStaticMeshEditorModule::DrawImportDialog(bool bAllowAssetMutation) -> void
	{
		if (Integration->ImportDialog)
			Integration->ImportDialog->Draw(bAllowAssetMutation);
	}

	auto FStaticMeshEditorModule::ReimportAsset(std::string_view AssetPath,
		std::function<void(std::string)> ReportError) -> void
	{
		ReimportStaticMesh(AssetPath, std::move(ReportError));
	}

	auto FStaticMeshEditorModule::ReimportAssetFromFile(std::string_view AssetPath,
		std::function<void(std::string)> ReportError) -> void
	{
		auto Report = [&ReportError](std::string Message) {
			if (ReportError) ReportError(std::move(Message));
		};
		FAssetPath Path;
		if (!FAssetPath::TryCreate(AssetPath, Path))
		{
			Report("The selected StaticMesh path is invalid.");
			return;
		}
		DStaticMesh* Mesh = nullptr;
		const Asset::FAssetResult Load = Asset::LoadAsset(Path, Mesh);
		if (!Load || !Mesh)
		{
			Report(Load ? "The selected StaticMesh could not be loaded." : Load.Message);
			return;
		}
		FFileDialogRequest Request;
		Request.Title = "Reimport StaticMesh From File";
		Request.Filters = {{"Supported Meshes", "*.fbx;*.gltf;*.glb;*.obj"},
			{"FBX", "*.fbx"}, {"glTF", "*.gltf;*.glb"}, {"OBJ", "*.obj"}};
		const FFileDialogResult Selection = OpenFileDialog(Request);
		if (Selection.Status == EFileDialogStatus::Cancelled) return;
		if (Selection.Status == EFileDialogStatus::Error)
		{
			Report(Selection.ErrorMessage);
			return;
		}
		std::string Error;
		if (!AssetForge::Builtins::ReimportStaticMeshFromFile(
			*Mesh, Selection.FilePath, Error)) Report(std::move(Error));
		else (void)Asset::UnloadPackage(Path);
	}

}

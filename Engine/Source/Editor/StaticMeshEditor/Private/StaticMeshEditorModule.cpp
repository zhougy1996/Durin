#include "StaticMeshEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Widgets/MStaticMeshInspector.h"
#include "Workspace/StaticMeshEditorWorkspace.h"
#include "Import/StaticMeshImportDialog.h"
#include "AssetForge/Builtins/StaticMeshImport.h"

namespace Durin
{
	using namespace Editor::StaticMesh;
	namespace
	{
		auto ReimportStaticMesh(
			const Editor::ContentBrowser::FExtensionInvocation& Invocation) -> void
		{
			auto ReportError = [&Invocation](std::string Message) {
				if (Invocation.ReportError)
					Invocation.ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Invocation.Context.AssetPath, Path))
			{
				ReportError("The selected StaticMesh path is invalid.");
				return;
			}
			DStaticMesh* Mesh = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Mesh);
			if (!Load || !Mesh)
			{
				ReportError(Load ? "The selected StaticMesh could not be loaded."
					: Load.Message);
				return;
			}
			AssetForge::FImportProvenance Existing;
			std::string Error;
			if (!AssetForge::Builtins::InspectStaticMeshImportProvenance(
				*Mesh, Existing, Error))
			{
				ReportError(std::move(Error));
				return;
			}
			const FStaticMeshSourceDiagnostic Source =
				AssetForge::Builtins::InspectStaticMeshSource(*Mesh);
			if (Source.Status != EStaticMeshSourceStatus::Available)
			{
				ReportError(Source.Message.empty()
					? "The StaticMesh source is unavailable." : Source.Message);
				return;
			}
			AssetForge::FImportRequest Request;
			if (!AssetForge::Builtins::MakeStaticMeshImportRequest(
				Mesh->GetSourceImportData().SourcePath, Path, Mesh->GetImportSettings(),
				AssetForge::EImportMode::Reimport,
				{.OwnerId = std::format("StaticMeshEditor.Reimport:{}", Path.ToString()),
					.ConflictIdentities = {Path.ToString()}},
				std::move(Existing), Request, Error))
			{
				ReportError(std::move(Error));
				return;
			}
			if (!Invocation.SubmitImport)
			{
				ReportError("The Content Browser import submitter is unavailable.");
				return;
			}
			(void)Invocation.SubmitImport(std::move(Request),
				std::format("Reimport {}", Path.GetAssetName()));
		}
	}

	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	FStaticMeshEditorModule::FStaticMeshEditorModule() = default;
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
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService,
		::Durin::Editor::Import::FImportDialogCallbacks ImportCallbacks) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (ThumbnailRegistration && ThumbnailRegistration->IsValid())) return false;
		WorkspaceRegistration.reset();
		ThumbnailRegistration.reset();
		ImportDialog = std::make_unique<Editor::StaticMesh::FStaticMeshImportDialog>(
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
		if (!EditorExtensionCallbacks.IsValid()) return true;
		ContentBrowserImportExtension =
			::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = "static-mesh.import",
				.Label = "Static Mesh (Geometry Only)...",
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Import,
				.Order = 300,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [this](const auto& Invocation) {
					if (ImportDialog)
						ImportDialog->Open(Invocation.Context.VirtualDirectory);
				},
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
		if (!ContentBrowserImportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser StaticMesh import: {}", Error);
			UnregisterStaticMeshEditor();
			return false;
		}
		ContentBrowserReimportExtension =
			::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = "static-mesh.reimport",
				.Label = "Reimport from Current Source",
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Reimport,
				.Order = 300,
				.IsApplicable = [](const auto& Context) {
					return Context.AssetClassName
						== DStaticMesh::StaticClass()->GetQualifiedName().ToString();
				},
				.Invoke = ReimportStaticMesh,
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
		if (!ContentBrowserReimportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser StaticMesh reimport: {}", Error);
			UnregisterStaticMeshEditor();
			return false;
		}
		return true;
	}

	auto FStaticMeshEditorModule::UnregisterStaticMeshEditor() -> void
	{
		ContentBrowserReimportExtension.Reset();
		ContentBrowserImportExtension.Reset();
		ImportDialog.reset();
		ThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

	auto FStaticMeshEditorModule::DrawImportDialogs() -> void
	{
		if (ImportDialog) ImportDialog->Draw();
	}
}

#include "TextureEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "Texture2DPropertyEditing.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Thumbnail/AssetThumbnailProvider.h"
#include "Thumbnail/Texture2DAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Widgets/MTextureEditor.h"
#include "Widgets/MVolumeTextureEditor.h"
#include "Widgets/TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"
#include "Workspace/VolumeTextureEditorWorkspace.h"
#include "Import/TextureImportDialog.h"
#include "Asset/Load.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "Dialogs/FileDialog.h"

namespace Durin
{
	using namespace Editor::Texture;
	namespace
	{
		auto ReimportTexture(std::string_view AssetPath,
			std::function<void(std::string)> ReportError) -> void
		{
			auto Report = [&ReportError](std::string Message) {
				if (ReportError) ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(AssetPath, Path))
			{
				Report("The selected texture path is invalid.");
				return;
			}
			DObject* Object = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Object);
			if (!Load || !Object)
			{
				Report(Load ? "The selected texture could not be loaded."
					: Load.Message);
				return;
			}
			std::string Error;
			if (auto* Texture = Cast<DTexture2D>(Object))
			{
				auto AsyncErrorReporter = ReportError;
				if (!AssetForge::Builtins::ReimportTexture2D(
					*Texture, Error,
					[AsyncErrorReporter = std::move(AsyncErrorReporter)](
						Asset::FTexture2DCompilationResult Result) {
						if (!Result.Succeeded() && AsyncErrorReporter)
							AsyncErrorReporter(Result.Diagnostic.empty()
								? "Texture2D reimport failed." : std::move(Result.Diagnostic));
					}))
				{
					Report(std::move(Error));
				}
				return;
			}
			else if (auto* Cube = Cast<DTextureCube>(Object))
			{
				bool Reimported = false;
				if (Cube->GetSourceLayout()
					== ETextureCubeSourceLayout::EquirectangularPanorama)
					Reimported = AssetForge::Builtins::ReimportTextureCubePanorama(
						*Cube, {.FaceDimension = Cube->GetPanoramaFaceDimension(),
							.ExposureEV = Cube->GetPanoramaExposureEV()}, Error);
				else
					Reimported = AssetForge::Builtins::ReimportTextureCubeFaces(
						*Cube, {.bSRGB = Cube->IsSRGB()}, Error);
				if (!Reimported) Report(std::move(Error));
				else (void)Asset::UnloadPackage(Path);
				return;
			}
			else if (auto* Volume = Cast<DVolumeTexture>(Object))
			{
				if (!AssetForge::Builtins::ReimportVolumeTexture(
					*Volume, Error)) Report(std::move(Error));
				else (void)Asset::UnloadPackage(Path);
				return;
			}
			else
			{
				Report("The selected asset is not a supported texture type.");
				return;
			}
		}

		auto ReimportTextureFromFile(std::string_view AssetPath,
			std::function<void(std::string)> ReportError) -> void
		{
			auto Report = [&ReportError](std::string Message) {
				if (ReportError) ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(AssetPath, Path))
			{
				Report("The selected texture path is invalid.");
				return;
			}
			DObject* Object = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Object);
			if (!Load || !Object)
			{
				Report(Load ? "The selected texture could not be loaded." : Load.Message);
				return;
			}
			auto SelectImage = [&](std::string Title, std::string& OutFile) -> bool {
				FFileDialogRequest Request;
				Request.Title = std::move(Title);
				Request.Filters = {{"Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr"},
					{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Targa", "*.tga"},
					{"HDR", "*.hdr"}};
				const FFileDialogResult Selection = OpenFileDialog(Request);
				if (Selection.Status == EFileDialogStatus::Cancelled) return false;
				if (Selection.Status == EFileDialogStatus::Error)
				{
					Report(Selection.ErrorMessage);
					return false;
				}
				OutFile = Selection.FilePath;
				return true;
			};
			std::string Error;
			if (auto* Texture = Cast<DTexture2D>(Object))
			{
				std::string File;
				if (!SelectImage("Reimport Texture2D From File", File)) return;
				auto AsyncErrorReporter = ReportError;
				if (!AssetForge::Builtins::ReimportTexture2DFromFile(*Texture, File, Error,
					[AsyncErrorReporter = std::move(AsyncErrorReporter)](
						Asset::FTexture2DCompilationResult Result) {
						if (!Result.Succeeded() && AsyncErrorReporter)
							AsyncErrorReporter(Result.Diagnostic.empty()
								? "Texture2D reimport from file failed."
								: std::move(Result.Diagnostic));
					})) Report(std::move(Error));
				return;
			}
			if (auto* Cube = Cast<DTextureCube>(Object))
			{
				bool bSucceeded = false;
				if (Cube->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					std::string File;
					if (!SelectImage("Reimport TextureCube Panorama From File", File)) return;
					bSucceeded = AssetForge::Builtins::ReimportTextureCubePanoramaFromFile(
						*Cube, File, {.FaceDimension = Cube->GetPanoramaFaceDimension(),
							.ExposureEV = Cube->GetPanoramaExposureEV()}, Error);
				}
				else
				{
					constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames{
						"Positive X", "Negative X", "Positive Y", "Negative Y",
						"Positive Z", "Negative Z"};
					std::array<std::string, TextureCubeFaceCount> Files;
					for (size_t Index = 0; Index < Files.size(); ++Index)
						if (!SelectImage(std::format("Reimport TextureCube {} Face From File",
							FaceNames[Index]), Files[Index])) return;
					bSucceeded = AssetForge::Builtins::ReimportTextureCubeFacesFromFile(
						*Cube, Files, {.bSRGB = Cube->IsSRGB()}, Error);
				}
				if (!bSucceeded) Report(std::move(Error));
				else (void)Asset::UnloadPackage(Path);
				return;
			}
			if (auto* Volume = Cast<DVolumeTexture>(Object))
			{
				std::string File;
				if (!SelectImage("Reimport VolumeTexture Atlas From File", File)) return;
				if (!AssetForge::Builtins::ReimportVolumeTextureFromFile(
					*Volume, File, Error)) Report(std::move(Error));
				else (void)Asset::UnloadPackage(Path);
				return;
			}
			Report("The selected asset is not a supported texture type.");
		}
	}

	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	struct FTextureEditorModule::FIntegrationState
	{
		std::unique_ptr<Editor::Texture::FTextureImportDialog> ImportDialog;
	};

	FTextureEditorModule::FTextureEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}
	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
		require(Editor::Texture::RegisterTexture2DPropertyEditing());
	}

	auto FTextureEditorModule::ShutdownModule() -> void
	{
		UnregisterTextureEditor();
		Editor::Texture::UnregisterTexture2DPropertyEditing();
		FTexturePreview::ReleaseSharedResources();
	}

	auto FTextureEditorModule::RegisterTextureEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
		::Durin::Editor::FImportDialogCallbacks ImportCallbacks) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (Texture2DThumbnailRegistration && Texture2DThumbnailRegistration->IsValid())
			|| (TextureCubeThumbnailRegistration && TextureCubeThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		TextureCubeThumbnailRegistration.reset();
		Integration->ImportDialog = std::make_unique<Editor::Texture::FTextureImportDialog>(
			std::move(ImportCallbacks));
		std::shared_ptr<MTextureEditor> Workspace = std::make_shared<MTextureEditor>(WorkspaceManager);
		std::shared_ptr<MVolumeTextureEditor> VolumeEditor =
			std::make_shared<MVolumeTextureEditor>(WorkspaceManager);
		::Durin::Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = Workspace::Type,
						.DisplayName = "Texture Editor",
						.RootKey = std::string(Workspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
				{
					.Descriptor = {
						.WorkspaceType = VolumeWorkspace::Type,
						.DisplayName = "Texture Editor",
						.RootKey = std::string(VolumeWorkspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = VolumeEditor,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
				{
					.AssetClassName = DVolumeTexture::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = VolumeWorkspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		}, EditorExtensionCallbacks.GetGate());
		if (!Registration)
		{
			UnregisterTextureEditor();
			return false;
		}
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto Texture2DHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTexture2DAssetThumbnailProvider>(),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!Texture2DHandle)
		{
			UnregisterTextureEditor();
			return false;
		}
		Texture2DThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(Texture2DHandle));
		auto TextureCubeHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTextureCubeAssetThumbnailProvider>(),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!TextureCubeHandle)
		{
			UnregisterTextureEditor();
			return false;
		}
		TextureCubeThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(TextureCubeHandle));
		return true;
	}

	auto FTextureEditorModule::UnregisterTextureEditor() -> void
	{
		Integration->ImportDialog.reset();
		TextureCubeThumbnailRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

	auto FTextureEditorModule::OpenImportDialog(std::string_view Directory) -> void
	{
		if (Integration->ImportDialog) Integration->ImportDialog->Open(Directory);
	}

	auto FTextureEditorModule::DrawImportDialog(bool bAllowAssetMutation) -> void
	{
		if (Integration->ImportDialog)
			Integration->ImportDialog->Draw(bAllowAssetMutation);
	}

	auto FTextureEditorModule::ReimportAsset(std::string_view AssetPath,
		std::function<void(std::string)> ReportError) -> void
	{
		ReimportTexture(AssetPath, std::move(ReportError));
	}

	auto FTextureEditorModule::ReimportAssetFromFile(std::string_view AssetPath,
		std::function<void(std::string)> ReportError) -> void
	{
		ReimportTextureFromFile(AssetPath, std::move(ReportError));
	}

}

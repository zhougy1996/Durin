#include "TextureEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "Texture2DPropertyEditing.h"
#include "TextureSourceRelocation.h"
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

namespace Durin
{
	using namespace Editor::Texture;
	namespace
	{
		auto ReimportTexture(
			const Editor::ContentBrowser::FExtensionInvocation& Invocation) -> void
		{
			auto ReportError = [&Invocation](std::string Message) {
				if (Invocation.ReportError)
					Invocation.ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Invocation.Context.AssetPath, Path))
			{
				ReportError("The selected texture path is invalid.");
				return;
			}
			DObject* Object = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Object);
			if (!Load || !Object)
			{
				ReportError(Load ? "The selected texture could not be loaded."
					: Load.Message);
				return;
			}
			AssetForge::FImportRequest Request;
			std::string Error;
			if (auto* Texture = Cast<DTexture2D>(Object))
			{
				AssetForge::FImportProvenance Existing;
				if (!AssetForge::Builtins::InspectTexture2DImportProvenance(
					*Texture, Existing, Error))
				{
					ReportError(std::move(Error));
					return;
				}
				const FTextureSourceDiagnostic Source = Texture->InspectSource();
				if (Source.Status != ETextureSourceStatus::Available)
				{
					ReportError(Source.Message.empty()
						? "The Texture2D source is unavailable." : Source.Message);
					return;
				}
				const FTexture2DImportSettings Settings{
					.Usage = Texture->GetUsage(),
					.CompressionQuality = Texture->GetCompressionQuality(),
					.AlphaMipMode = Texture->GetAlphaMipMode(),
					.AlphaCoverageThreshold = Texture->GetAlphaCoverageThreshold(),
					.MaxResolution = Texture->GetMaxResolution(),
					.bSRGB = Texture->IsSRGB()};
				if (!AssetForge::Builtins::MakeTexture2DImportRequest(
					Texture->GetSourceImportData().Source.SourcePath, Path, Settings,
					AssetForge::EImportMode::Reimport,
					{.OwnerId = std::format("TextureEditor.Reimport:{}", Path.ToString()),
						.ConflictIdentities = {Path.ToString()}},
					std::move(Existing), Request, Error))
				{
					ReportError(std::move(Error));
					return;
				}
			}
			else if (auto* Cube = Cast<DTextureCube>(Object))
			{
				AssetForge::FImportProvenance Existing;
				if (!AssetForge::Builtins::InspectTextureCubeImportProvenance(
					*Cube, Existing, Error))
				{
					ReportError(std::move(Error));
					return;
				}
				std::array<FSourcePath, TextureCubeFaceCount> Sources;
				const size_t SourceCount = Cube->GetSourceLayout()
					== ETextureCubeSourceLayout::SixFaces ? TextureCubeFaceCount : 1;
				if (SourceCount == 1)
					Sources[0] = Cube->GetSourceImportData().Panorama.SourcePath;
				else
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
						Sources[Index] = Cube->GetSourceImportData().GetFace(
							static_cast<ETextureCubeFace>(Index)).SourcePath;
				if (!AssetForge::Builtins::MakeTextureCubeImportRequest(
					std::span(Sources).first(SourceCount), Cube->GetSourceLayout(), Path,
					{.bSRGB = Cube->IsSRGB()},
					{.FaceDimension = Cube->GetPanoramaFaceDimension(),
						.ExposureEV = Cube->GetPanoramaExposureEV()},
					AssetForge::EImportMode::Reimport,
					{.OwnerId = std::format("TextureEditor.Reimport:{}", Path.ToString()),
						.ConflictIdentities = {Path.ToString()}},
					std::move(Existing), Request, Error))
				{
					ReportError(std::move(Error));
					return;
				}
			}
			else if (auto* Volume = Cast<DVolumeTexture>(Object))
			{
				AssetForge::FImportProvenance Existing;
				if (!AssetForge::Builtins::InspectVolumeTextureImportProvenance(
					*Volume, Existing, Error))
				{
					ReportError(std::move(Error));
					return;
				}
				const FVolumeTextureSourceImportData& Source =
					Volume->GetSourceImportData();
				const AssetForge::Builtins::FVolumeTextureImportSettings Settings{
					.ImportFormat = Source.ImportFormat,
					.Channels = Source.Channels,
					.SliceWidth = Source.SliceWidth,
					.SliceHeight = Source.SliceHeight,
					.Depth = Source.Depth,
					.TilesX = Source.TilesX,
					.TilesY = Source.TilesY};
				if (!AssetForge::Builtins::MakeVolumeTextureImportRequest(
					Source.Source.SourcePath, Path, Settings,
					AssetForge::EImportMode::Reimport,
					{.OwnerId = std::format("TextureEditor.Reimport:{}", Path.ToString()),
						.ConflictIdentities = {Path.ToString()}},
					std::move(Existing), Request, Error))
				{
					ReportError(std::move(Error));
					return;
				}
			}
			else
			{
				ReportError("The selected asset is not a supported texture type.");
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

	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	FTextureEditorModule::FTextureEditorModule() = default;
	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
		require(Editor::Texture::RegisterTexture2DPropertyEditing());
		require(Editor::Texture::RegisterTextureSourceRelocation());
	}

	auto FTextureEditorModule::ShutdownModule() -> void
	{
		UnregisterTextureEditor();
		Editor::Texture::UnregisterTextureSourceRelocation();
		Editor::Texture::UnregisterTexture2DPropertyEditing();
		FTexturePreview::ReleaseSharedResources();
	}

	auto FTextureEditorModule::RegisterTextureEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
		::Durin::Editor::Import::FImportDialogCallbacks ImportCallbacks) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (Texture2DThumbnailRegistration && Texture2DThumbnailRegistration->IsValid())
			|| (TextureCubeThumbnailRegistration && TextureCubeThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		TextureCubeThumbnailRegistration.reset();
		ImportDialog = std::make_unique<Editor::Texture::FTextureImportDialog>(
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
		if (!EditorExtensionCallbacks.IsValid()) return true;
		ContentBrowserImportExtension =
			::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = "texture.import", .Label = "Texture...",
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Import,
				.Order = 100,
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
			DURIN_ERROR("Could not register Content Browser texture import: {}", Error);
			UnregisterTextureEditor();
			return false;
		}
		ContentBrowserReimportExtension =
			::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = "texture.reimport",
				.Label = "Reimport from Current Source",
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Reimport,
				.Order = 200,
				.IsApplicable = [](const auto& Context) {
					const std::string& Class = Context.AssetClassName;
					return Class == DTexture2D::StaticClass()->GetQualifiedName().ToString()
						|| Class == DTextureCube::StaticClass()->GetQualifiedName().ToString()
						|| Class == DVolumeTexture::StaticClass()->GetQualifiedName().ToString();
				},
				.Invoke = ReimportTexture,
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
		if (!ContentBrowserReimportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser texture reimport: {}", Error);
			UnregisterTextureEditor();
			return false;
		}
		return true;
	}

	auto FTextureEditorModule::UnregisterTextureEditor() -> void
	{
		ContentBrowserReimportExtension.Reset();
		ContentBrowserImportExtension.Reset();
		ImportDialog.reset();
		TextureCubeThumbnailRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

	auto FTextureEditorModule::DrawImportDialogs() -> void
	{
		if (ImportDialog) ImportDialog->Draw();
	}
}

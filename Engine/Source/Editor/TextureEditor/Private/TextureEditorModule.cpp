#include "TextureEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"
#include "ContentBrowser/TextureCubeDetails.h"
#include "Editor/WorkspaceManager.h"
#include "Texture2DPropertyEditing.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Widgets/MTextureEditor.h"
#include "Widgets/MVolumeTextureEditor.h"
#include "Widgets/TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"
#include "Workspace/VolumeTextureEditorWorkspace.h"
#include "Import/TextureImportDialog.h"

namespace Durin
{
	using namespace Editor::Texture;
	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	struct FTextureEditorModule::FIntegrationState
	{
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration> TypePresentations;
		Editor::ContentBrowser::FScopedExtensionRegistration ImportExtension;
		std::unique_ptr<Editor::Texture::FTextureImportDialog> ImportDialog;
	};

	FTextureEditorModule::FTextureEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}
	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule() -> void
	{
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
		::Durin::Editor::DThumbnailManager& ThumbnailManager,
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
		});
		if (!Registration)
		{
			UnregisterTextureEditor();
			return false;
		}
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto Texture2DHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DTextureThumbnailRenderer>(),
			Error);
		if (!Texture2DHandle)
		{
			UnregisterTextureEditor();
			return false;
		}
		Texture2DThumbnailRegistration =
			std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(Texture2DHandle));
		auto TextureCubeHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DTextureCubeThumbnailRenderer>(),
			Error);
		if (!TextureCubeHandle)
		{
			UnregisterTextureEditor();
			return false;
		}
		TextureCubeThumbnailRegistration =
			std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(TextureCubeHandle));
		auto ImportExtension = Editor::ContentBrowser::RegisterExtension({
			.Id = "texture.import-texture",
			.Label = "Texture...",
			.Category = Editor::ContentBrowser::EExtensionCategory::Import,
			.Order = 100,
			.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
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
			}, Error);
		if (!ImportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser Texture import: {}", Error);
			UnregisterTextureEditor();
			return false;
		}
		Integration->ImportExtension = std::move(ImportExtension);
		std::string PresentationError;
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Texture2D",
				.Category = Editor::ContentBrowser::EAssetCategory::Texture,
				.Icon = Icons::FileLines,
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterTextureEditor();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Texture Cube",
				.Category = Editor::ContentBrowser::EAssetCategory::Texture,
				.Icon = Icons::Cube,
				.ThumbnailBadgeIcon = Icons::Cube,
				.Details = Editor::Texture::MakeTextureCubeDetailsProvider(),
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterTextureEditor();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DVolumeTexture::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Volume Texture",
				.Category = Editor::ContentBrowser::EAssetCategory::Texture,
				.Icon = Icons::Cube,
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterTextureEditor();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		return true;
	}

	auto FTextureEditorModule::UnregisterTextureEditor() -> void
	{
		Integration->TypePresentations.clear();
		Integration->ImportExtension.Reset();
		Integration->ImportDialog.reset();
		TextureCubeThumbnailRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}

}

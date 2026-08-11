#include "TextureEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "Texture/Texture2D.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/Texture2DAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Widgets/MTextureEditor.h"
#include "Widgets/TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"

namespace Durin
{
	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule() -> void
	{
	}

	auto FTextureEditorModule::ShutdownModule() -> void
	{
		UnregisterTextureEditor();
		FTexturePreview::ReleaseSharedResources();
	}

	auto FTextureEditorModule::RegisterTextureEditor(
		Editor::FWorkspaceManager& WorkspaceManager,
		FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (Texture2DThumbnailRegistration && Texture2DThumbnailRegistration->IsValid())
			|| (TextureCubeThumbnailRegistration && TextureCubeThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		TextureCubeThumbnailRegistration.reset();
		std::shared_ptr<MTextureEditor> Workspace = std::make_shared<MTextureEditor>(WorkspaceManager);
		Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = TextureEditorWorkspace::Type,
						.DisplayName = "Texture Editor",
						.RootKey = std::string(TextureEditorWorkspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = TextureEditorWorkspace::Type,
					.DocumentPolicy = Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto Texture2DHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTexture2DAssetThumbnailProvider>(), Error);
		if (!Texture2DHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		Texture2DThumbnailRegistration =
			std::make_unique<FAssetThumbnailProviderRegistrationHandle>(
				std::move(Texture2DHandle));
		auto TextureCubeHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTextureCubeAssetThumbnailProvider>(), Error);
		if (!TextureCubeHandle)
		{
			Texture2DThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		TextureCubeThumbnailRegistration =
			std::make_unique<FAssetThumbnailProviderRegistrationHandle>(
				std::move(TextureCubeHandle));
		return true;
	}

	auto FTextureEditorModule::UnregisterTextureEditor() -> void
	{
		TextureCubeThumbnailRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}

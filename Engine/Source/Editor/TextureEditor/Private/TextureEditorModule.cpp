#include "TextureEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "Texture/Texture2D.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "Thumbnail/Texture2DAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Widgets/MTextureEditor.h"
#include "Widgets/TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"

namespace Durin
{
	using namespace Editor::Texture;

	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule(FModuleContext& Context) -> void
	{
		EditorExtensionCallbacks =
			Context.CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
	}

	auto FTextureEditorModule::ShutdownModule(FModuleShutdownContext&) -> void
	{
		UnregisterTextureEditor();
		FTexturePreview::ReleaseSharedResources();
	}

	auto FTextureEditorModule::RegisterTextureEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (Texture2DThumbnailRegistration && Texture2DThumbnailRegistration->IsValid())
			|| (TextureCubeThumbnailRegistration && TextureCubeThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		Texture2DThumbnailRegistration.reset();
		TextureCubeThumbnailRegistration.reset();
		std::shared_ptr<MTextureEditor> Workspace = std::make_shared<MTextureEditor>(WorkspaceManager);
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
			},
			.AssetEditors = {
				{
					.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		}, EditorExtensionCallbacks.GetGate());
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto Texture2DHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTexture2DAssetThumbnailProvider>(),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!Texture2DHandle)
		{
			WorkspaceRegistration.reset();
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
			Texture2DThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		TextureCubeThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
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

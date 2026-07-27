#include "TextureEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "Texture/Texture2D.h"
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
		UnregisterTextureEditorWorkspace();
		FTexturePreview::ReleaseSharedResources();
	}

	auto FTextureEditorModule::RegisterTextureEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
		std::shared_ptr<MTextureEditor> Workspace = std::make_shared<MTextureEditor>(WorkspaceManager);
		FEditorWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = TextureEditorWorkspace::Type,
						.DisplayName = "Texture Editor",
						.RootKey = std::string(TextureEditorWorkspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = EEditorWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = TextureEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<FEditorWorkspaceRegistrationHandle>(std::move(Registration));
		return true;
	}

	auto FTextureEditorModule::UnregisterTextureEditorWorkspace() -> void
	{
		WorkspaceRegistration.reset();
	}
}

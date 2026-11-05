#pragma once

#include "CoreMinimal.h"
#include "Workspace/WorkspaceProvider.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"
#include "Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public IModuleInterface, public Editor::IWorkspaceProvider
	{
	public:
		TEXTUREEDITOR_API FTextureEditorModule();
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterWorkspaces(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::DThumbnailManager& ThumbnailManager,
			::Durin::Editor::FImportDialogCallbacks ImportCallbacks = {}) -> bool override;
		TEXTUREEDITOR_API auto UnregisterWorkspaces() -> void override;
	private:
		Editor::FDetailCustomizationHandle DetailCustomizationRegistration;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FThumbnailRendererRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<::Durin::Editor::FThumbnailRendererRegistrationHandle> TextureCubeThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
		FModularFeatureRegistration ProviderRegistration;
	};
}

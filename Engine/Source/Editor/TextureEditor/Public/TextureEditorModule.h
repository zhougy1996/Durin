#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorModule.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"

#include "Modules/ModuleManager.h"
#include "TextureEditorAPI.h"
#include "Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FAssetEditorToolkitRegistrationHandle;
	class FEditorDocumentManager;
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Registers the texture workspace and texture asset-editor mapping.
	class FTextureEditorModule final : public Editor::IAssetEditorModule
	{
	public:
		TEXTUREEDITOR_API FTextureEditorModule();
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
		TEXTUREEDITOR_API auto RegisterToolkits(
			Editor::FEditorDocumentManager& DocumentManager,
			Editor::DThumbnailManager& ThumbnailManager,
			Editor::FImportDialogCallbacks ImportCallbacks = {}) -> bool override;
		TEXTUREEDITOR_API auto UnregisterToolkits() -> void override;
	private:
		Editor::FDetailCustomizationHandle DetailCustomizationRegistration;
		std::unique_ptr<Editor::FAssetEditorToolkitRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> TextureCubeThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}

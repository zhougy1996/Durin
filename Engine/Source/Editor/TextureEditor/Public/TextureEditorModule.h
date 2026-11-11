#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"

#include "TextureEditorAPI.h"

namespace Durin
{
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Publishes independent texture definitions, import, details and thumbnails.
	class FTextureEditorModule final : public IModuleInterface
	{
	public:
		TEXTUREEDITOR_API FTextureEditorModule();
		TEXTUREEDITOR_API ~FTextureEditorModule() override;
		TEXTUREEDITOR_API auto StartupModule() -> void override;
		TEXTUREEDITOR_API auto ShutdownModule() -> void override;
	private:
		auto RegisterContributions(DThumbnailManager& ThumbnailManager) -> bool;
		auto UnregisterContributions() -> void;
		FDetailCustomizationHandle DetailCustomizationRegistration;
		std::unique_ptr<FThumbnailRendererRegistrationHandle> Texture2DThumbnailRegistration;
		std::unique_ptr<FThumbnailRendererRegistrationHandle> TextureCubeThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}

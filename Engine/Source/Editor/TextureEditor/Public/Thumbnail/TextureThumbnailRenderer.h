#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "TextureEditorAPI.h"

#include "TextureThumbnailRenderer.gen.h"

namespace Durin::TextureEditor
{
	// Captures built GPU texture output through the shared thumbnail scheduler.
	DCLASS()
	class DTextureThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		TEXTUREEDITOR_API explicit DTextureThumbnailRenderer(const FObjectInitializer& Initializer);
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailGenerationData, std::string> override;
		TEXTUREEDITOR_API auto CreateGenerationSession(const IAssetThumbnailGenerationInput& Input)
			-> std::unique_ptr<IThumbnailRendererSession> override;
	};
} // namespace Durin::TextureEditor

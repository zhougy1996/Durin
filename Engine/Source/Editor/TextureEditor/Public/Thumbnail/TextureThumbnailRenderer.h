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
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration)
			-> std::expected<FAssetThumbnailGenerationRequest, std::string> override;
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input)
			-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string> override;
	};
} // namespace Durin::TextureEditor

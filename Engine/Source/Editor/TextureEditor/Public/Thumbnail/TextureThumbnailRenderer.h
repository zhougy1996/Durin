#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Captures built GPU texture output through the shared thumbnail scheduler.
	class DTextureThumbnailRenderer final : public DDefaultSizedThumbnailRenderer
	{
	public:
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
} // namespace Durin::Editor::Texture

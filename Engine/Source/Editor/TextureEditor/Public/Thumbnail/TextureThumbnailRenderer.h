#pragma once

#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Produces fixed output from canonical pixels stored in the authored package.
	class DTextureThumbnailRenderer final : public ::Durin::Editor::DDefaultSizedThumbnailRenderer
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 RendererGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
	};
} // namespace Durin::Editor::Texture

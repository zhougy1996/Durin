#pragma once

#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Captures built GPU texture output through the shared thumbnail scheduler.
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
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError) -> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession> override;
	};
} // namespace Durin::Editor::Texture

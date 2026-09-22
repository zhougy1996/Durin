#pragma once

#include <expected>

#include "MaterialEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"

namespace Durin { class DMaterialInterface; }

namespace Durin::Editor::Material
{
	// Captures deterministic material dependency keys for one exact material asset class.
	class DMaterialThumbnailRenderer final : public ::Durin::Editor::DDefaultSizedThumbnailRenderer
	{
	public:
		MATERIALEDITOR_API explicit DMaterialThumbnailRenderer(std::string AssetClassName);

		MATERIALEDITOR_API auto GetRegistration() const -> ::Durin::Editor::FThumbnailRenderingInfo override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 RendererGeneration)
			-> std::expected<::Durin::Editor::FAssetThumbnailGenerationRequest, std::string> override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input)
			-> std::expected<std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>, std::string> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin::Editor::Material

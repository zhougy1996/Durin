#pragma once

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
			uint64 RendererGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin::Editor::Material

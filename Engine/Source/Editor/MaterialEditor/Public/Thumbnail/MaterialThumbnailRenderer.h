#pragma once

#include "CoreMinimal.h"

#include "MaterialEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"

namespace Durin { class DMaterialInterface; }

namespace Durin::MaterialEditor
{
	// Captures deterministic material dependency keys for one exact material asset class.
	class DMaterialThumbnailRenderer final : public DDefaultSizedThumbnailRenderer
	{
	public:
		MATERIALEDITOR_API explicit DMaterialThumbnailRenderer(std::string AssetClassName);

		MATERIALEDITOR_API auto GetRegistration() const -> FThumbnailRenderingInfo override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration)
			-> std::expected<FAssetThumbnailGenerationRequest, std::string> override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input)
			-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin::MaterialEditor

#pragma once

#include "CoreMinimal.h"

#include "MaterialEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"

#include "MaterialThumbnailRenderer.gen.h"

namespace Durin { class DMaterialInterface; }

namespace Durin::MaterialEditor
{
	// Captures deterministic material dependency keys for one exact material asset class.
	DCLASS()
	class DMaterialThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		MATERIALEDITOR_API explicit DMaterialThumbnailRenderer(const FObjectInitializer& Initializer);

		MATERIALEDITOR_API auto GetRegistration() const -> FThumbnailRenderingInfo override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration)
			-> std::expected<FAssetThumbnailGenerationRequest, std::string> override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input)
			-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string> override;

	};
} // namespace Durin::MaterialEditor

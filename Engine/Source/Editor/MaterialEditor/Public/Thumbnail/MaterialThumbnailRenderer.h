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
		MATERIALEDITOR_API auto CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailGenerationData, std::string> override;
		MATERIALEDITOR_API auto CreateGenerationSession(const IAssetThumbnailGenerationInput& Input)
			-> std::unique_ptr<IThumbnailRendererSession> override;

	};
} // namespace Durin::MaterialEditor

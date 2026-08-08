#pragma once

#include "MaterialEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin
{
	class DMaterialInterface;

	// Captures deterministic material dependency keys for one exact material asset class.
	class FMaterialAssetThumbnailProvider final : public IRenderedAssetThumbnailExtension
	{
	public:
		MATERIALEDITOR_API explicit FMaterialAssetThumbnailProvider(std::string AssetClassName);

		MATERIALEDITOR_API auto GetRegistration() const -> FAssetThumbnailProviderRegistration override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<IRenderedAssetThumbnailGenerationSession> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin

#pragma once

#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	// Immutable provider input used by the shared rendered-thumbnail scheduler.
	class FTextureCubeThumbnailGenerationInput final
		: public IAssetThumbnailGenerationInput
	{
	public:
		explicit FTextureCubeThumbnailGenerationInput(FAssetPath InAssetPath)
			: AssetPath(std::move(InAssetPath))
		{
		}

		FAssetPath AssetPath;
	};

	// Captures the exact TextureCube package identity and preview visual contract.
	class FTextureCubeAssetThumbnailProvider final : public IAssetThumbnailProvider
	{
	public:
		DURINED_API auto GetRegistration() const
			-> FAssetThumbnailProviderRegistration override;
		DURINED_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
	};
} // namespace Durin

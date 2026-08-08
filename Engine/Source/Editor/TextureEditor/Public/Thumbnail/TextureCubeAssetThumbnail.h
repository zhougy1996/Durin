#pragma once

#include "Thumbnail/RenderedAssetThumbnailExtension.h"
#include "TextureEditorAPI.h"

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
	class FTextureCubeAssetThumbnailProvider final : public IRenderedAssetThumbnailExtension
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FAssetThumbnailProviderRegistration override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<IRenderedAssetThumbnailGenerationSession> override;
	};
} // namespace Durin

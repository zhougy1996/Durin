#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	// Resolves an authored Texture2D package to its supported source-image preview.
	// Decode, persistence, upload, and card presentation remain provider-neutral.
	class FTexture2DAssetThumbnailProvider final : public Editor::IAssetThumbnailProvider
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> Editor::FAssetThumbnailProviderRegistration override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		auto UsesSourceImage() const -> bool override { return true; }
		TEXTUREEDITOR_API auto CaptureSourceImage(
			const Asset::FAssetData& Asset,
			Editor::FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) -> bool override;
	};
} // namespace Durin

#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Resolves an authored Texture2D package to its supported source-image preview.
	// Decode, persistence, upload, and card presentation remain provider-neutral.
	class FTexture2DAssetThumbnailProvider final : public ::Durin::Editor::IAssetThumbnailProvider
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FAssetThumbnailProviderRegistration override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		auto UsesSourceImage() const -> bool override { return true; }
		TEXTUREEDITOR_API auto CaptureSourceImage(
			const Asset::FAssetData& Asset,
			::Durin::Editor::FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) -> bool override;
	};
} // namespace Durin::Editor::Texture

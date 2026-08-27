#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Keeps Texture2D thumbnail routing source-independent. Until canonical-pixel
	// generation is implemented, the browser falls back to the asset icon.
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
		auto UsesSourceImage() const -> bool override { return false; }
		TEXTUREEDITOR_API auto CaptureSourceImage(
			const Asset::FAssetData& Asset,
			::Durin::Editor::FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) -> bool override;
	};
} // namespace Durin::Editor::Texture

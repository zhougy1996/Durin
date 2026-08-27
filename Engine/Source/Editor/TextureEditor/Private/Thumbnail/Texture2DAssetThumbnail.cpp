#include "Thumbnail/Texture2DAssetThumbnail.h"

#include "Texture/Texture2D.h"

namespace Durin::Editor::Texture
{
	auto FTexture2DAssetThumbnailProvider::GetRegistration() const
		-> ::Durin::Editor::FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = "Texture2DSourceThumbnail",
			.GeneratorSchemaVersion = 1};
	}

	auto FTexture2DAssetThumbnailProvider::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest&,
		uint64,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError = "Texture2D canonical-pixel thumbnail generation is unavailable.";
		return false;
	}

	auto FTexture2DAssetThumbnailProvider::CaptureSourceImage(
		const Asset::FAssetData& AssetData,
		::Durin::Editor::FAssetThumbnailSourceImage& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		if (AssetData.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The Texture2D thumbnail provider received the wrong asset class.";
			return false;
		}
		OutError = "Texture2D source-image thumbnails are disabled; source hints are never probed implicitly.";
		return false;
	}
} // namespace Durin::Editor::Texture

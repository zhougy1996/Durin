#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"
#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin
{
	// Captured from the registry only while constructing a persistent cache key.
	struct FAssetThumbnailKeyInput
	{
		FAssetData Asset;
		std::string RendererClassName;
		FAssetThumbnailOutputSettings Output;
		std::vector<FPackageData> Packages;
	};

	DURINED_API auto CaptureThumbnailKeyInput(const FAssetData& Asset,
		const DClass* RendererClass)
		-> std::expected<FAssetThumbnailKeyInput, std::string>;
	DURINED_API auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string;
}

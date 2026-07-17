#pragma once

#include "AssetCoreAPI.h"
#include "HAL/Platform.h"

namespace Durin::Asset
{
	struct FDecodedImage
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;
	};

	ASSETCORE_API auto IsSupportedImageExtension(std::string_view Extension) -> bool;
	ASSETCORE_API auto DecodeImageFromMemory(std::span<const uint8> EncodedBytes, FDecodedImage& OutImage, std::string& OutError) -> bool;
	ASSETCORE_API auto DecodeImageFromFile(std::string_view FilePath, FDecodedImage& OutImage, std::string& OutError) -> bool;
} // namespace Durin::Asset

#pragma once

#include "AssetCoreAPI.h"
#include "HAL/Platform.h"

namespace Durin::Asset
{
	struct FImageDecodeLimits
	{
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;
		uint64 MaximumDecodedPixels = 256ull * 1024ull * 1024ull;
	};

	struct FDecodedImage
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;
	};

	ASSETCORE_API auto IsSupportedImageExtension(std::string_view Extension) -> bool;
	ASSETCORE_API auto DecodeImageFromMemory(std::span<const uint8> EncodedBytes, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	ASSETCORE_API auto DecodeImageFromFile(std::string_view FilePath, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
} // namespace Durin::Asset

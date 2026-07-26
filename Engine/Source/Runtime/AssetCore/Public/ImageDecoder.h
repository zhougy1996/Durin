#pragma once

#include "AssetCoreAPI.h"
#include "HAL/Platform.h"

namespace Durin::Asset
{
	// Bounds encoded input and decoded output before image memory is allocated.
	struct FImageDecodeLimits
	{
		// Maximum accepted encoded payload size in bytes.
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;

		// Maximum accepted width-by-height pixel count.
		uint64 MaximumDecodedPixels = 256ull * 1024ull * 1024ull;
	};

	// Stores decoded RGBA8 pixels and source-image metadata.
	struct FDecodedImage
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;

		// Channel count reported by the source before conversion to RGBA8.
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;
	};

	// Bounds Radiance HDR input and its decoded linear RGB working set.
	struct FRadianceHDRDecodeLimits
	{
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;
		uint64 MaximumDecodedPixels = 32ull * 1024ull * 1024ull;
		uint32 MaximumDimension = 16384;
	};

	// Stores top-left-origin, linear RGB float pixels decoded from Radiance HDR.
	struct FDecodedFloatImage
	{
		std::vector<float> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
	};

	ASSETCORE_API auto IsSupportedImageExtension(std::string_view Extension) -> bool;
	ASSETCORE_API auto IsRadianceHDRExtension(std::string_view Extension) -> bool;
	ASSETCORE_API auto DecodeImageFromMemory(std::span<const uint8> EncodedBytes, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	ASSETCORE_API auto DecodeImageFromFile(std::string_view FilePath, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	ASSETCORE_API auto DecodeRadianceHDRFromMemory(std::span<const uint8> EncodedBytes, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits = {}) -> bool;
	ASSETCORE_API auto DecodeRadianceHDRFromFile(std::string_view FilePath, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits = {}) -> bool;
} // namespace Durin::Asset

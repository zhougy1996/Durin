#pragma once

#include "CoreAPI.h"
#include "HAL/Platform.h"
#include "Image/Image.h"

namespace Durin::Image
{
	// Bounds encoded input and decoded output before image memory is allocated.
	struct FImageDecodeLimits
	{
		// Maximum accepted encoded payload size in bytes.
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;

		// Maximum accepted width-by-height pixel count.
		uint64 MaximumDecodedPixels = 256ull * 1024ull * 1024ull;
	};

	// Bounds Radiance HDR input and its decoded linear RGB working set.
	struct FRadianceHDRDecodeLimits
	{
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;
		uint64 MaximumDecodedPixels = 32ull * 1024ull * 1024ull;
		uint32 MaximumDimension = 16384;
	};

	CORE_API auto IsSupportedImageExtension(std::string_view Extension) -> bool;
	CORE_API auto IsRadianceHDRExtension(std::string_view Extension) -> bool;
	CORE_API auto DecodeImageFromMemory(FByteView EncodedBytes, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	CORE_API auto DecodeImageFromFile(std::string_view FilePath, FDecodedImage& OutImage, std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	CORE_API auto DecodeGrayscale16PngFromMemory(
		FByteView EncodedBytes,
		FDecodedGrayscale16Image& OutImage,
		std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	CORE_API auto DecodeGrayscale16PngFromFile(
		std::string_view FilePath,
		FDecodedGrayscale16Image& OutImage,
		std::string& OutError,
		const FImageDecodeLimits& Limits = {}) -> bool;
	CORE_API auto DecodeRadianceHDRFromMemory(FByteView EncodedBytes, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits = {}) -> bool;
	CORE_API auto DecodeRadianceHDRFromFile(std::string_view FilePath, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits = {}) -> bool;
} // namespace Durin::Image

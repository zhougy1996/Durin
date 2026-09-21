#pragma once

#include "CoreAPI.h"
#include "HAL/Platform.h"
#include "Image/Image.h"
#include "Misc/FileError.h"
#include <expected>

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

	enum class EImageDecodeError : uint8 { None, Empty, EncodedLimit, InvalidImage, PixelLimit, FileStat, FileSize, FileRead };
	struct FImageDecodeError
	{
		EImageDecodeError Code = EImageDecodeError::None;
		uint64 EncodedBytes = 0;
		int32 Width = 0;
		int32 Height = 0;
		FImageDecodeLimits Limits;
		std::string Filename;
		std::optional<FFileError> FileError;
	};
	CORE_API auto ToString(const FImageDecodeError& Error) -> std::string;

	// Bounds Radiance HDR input and its decoded linear RGB working set.
	struct FRadianceHDRDecodeLimits
	{
		uint64 MaximumEncodedBytes = 512ull * 1024ull * 1024ull;
		uint64 MaximumDecodedPixels = 32ull * 1024ull * 1024ull;
		uint32 MaximumDimension = 16384;
	};

	enum class EGrayscale16DecodeError : uint8
	{
		EncodedLimit,
		InvalidSignature,
		InvalidHeader,
		UnsupportedSampleFormat,
		UnsupportedEncoding,
		PixelLimit,
		InvalidImage,
		FileStat,
		FileSize,
		FileRead
	};
	struct FGrayscale16DecodeError
	{
		EGrayscale16DecodeError Code = EGrayscale16DecodeError::EncodedLimit;
		uint64 EncodedBytes = 0;
		uint32 Width = 0;
		uint32 Height = 0;
		FImageDecodeLimits Limits;
		std::string Filename;
		std::optional<FFileError> FileError;
	};
	CORE_API auto ToString(const FGrayscale16DecodeError& Error) -> std::string;

	enum class ERadianceHDRDecodeError : uint8
	{
		TruncatedScanline,
		ZeroLengthPacket,
		RunExceedsWidth,
		TruncatedRun,
		LiteralExceedsWidth,
		TruncatedLiteral,
		InvalidRepeat,
		RepeatExceedsWidth,
		TruncatedOldScanline,
		Empty,
		EncodedLimit,
		InvalidSignature,
		InvalidHeader,
		MissingFormat,
		MissingResolution,
		UnsupportedOrientation,
		DimensionLimit,
		PixelLimit,
		TruncatedScanlineHeader,
		ScanlineWidthMismatch,
		InvalidChannel,
		TrailingBytes,
		FileStat,
		FileSize,
		FileRead
	};
	struct FRadianceHDRDecodeError
	{
		ERadianceHDRDecodeError Code = ERadianceHDRDecodeError::TruncatedScanline;
		uint64 EncodedBytes = 0;
		uint64 Offset = 0;
		uint32 Width = 0;
		uint32 Height = 0;
		FRadianceHDRDecodeLimits Limits;
		std::string Filename;
		std::optional<FFileError> FileError;
	};
	CORE_API auto ToString(const FRadianceHDRDecodeError& Error) -> std::string;

	CORE_API auto IsSupportedImageExtension(std::string_view Extension) -> bool;
	CORE_API auto IsRadianceHDRExtension(std::string_view Extension) -> bool;
	[[nodiscard]] CORE_API auto DecodeImageFromMemory(FByteView EncodedBytes,
		const FImageDecodeLimits& Limits = {}) -> std::expected<FDecodedImage, FImageDecodeError>;
	[[nodiscard]] CORE_API auto DecodeImageFromFile(std::string_view FilePath,
		const FImageDecodeLimits& Limits = {}) -> std::expected<FDecodedImage, FImageDecodeError>;
	CORE_API auto DecodeGrayscale16PngFromMemory(
		FByteView EncodedBytes,
		const FImageDecodeLimits& Limits = {}) -> std::expected<FDecodedGrayscale16Image, FGrayscale16DecodeError>;
	CORE_API auto DecodeGrayscale16PngFromFile(
		std::string_view FilePath,
		const FImageDecodeLimits& Limits = {}) -> std::expected<FDecodedGrayscale16Image, FGrayscale16DecodeError>;
	CORE_API auto DecodeRadianceHDRFromMemory(FByteView EncodedBytes, const FRadianceHDRDecodeLimits& Limits = {})
		-> std::expected<FDecodedFloatImage, FRadianceHDRDecodeError>;
	CORE_API auto DecodeRadianceHDRFromFile(std::string_view FilePath, const FRadianceHDRDecodeLimits& Limits = {})
		-> std::expected<FDecodedFloatImage, FRadianceHDRDecodeError>;
} // namespace Durin::Image

#pragma once

#include "EngineAPI.h"
#include "Texture/Texture2D.h"

namespace Durin::TextureCubeLegacyBuild
{
	struct FBuildMipChainMetrics
	{
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PeakIntermediateBytes = 0;
	};

	struct FBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		FBuildMipChainMetrics* Metrics = nullptr;
	};

	inline constexpr uint32 ChannelCount = 4;
	inline constexpr uint32 MaxDimension = 16384;
	inline constexpr uint32 CancellationBlockInterval = 64;
	inline constexpr uint32 CancellationScanlineInterval = 8;

	ENGINE_API auto IsValidUsage(ETextureUsage Usage) -> bool;
	ENGINE_API auto GetDefaultSRGB(ETextureUsage Usage) -> bool;
	ENGINE_API auto IsValidCompressionQuality(ETextureCompressionQuality Quality) -> bool;
	ENGINE_API auto IsValidAlphaMipMode(ETextureAlphaMipMode Mode) -> bool;
	ENGINE_API auto IsValidAlphaCoverageThreshold(float Threshold) -> bool;
	ENGINE_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Decodes a supported source image into the canonical top-left-origin RGBA8 representation.
	ENGINE_API auto DecodeRGBA8(std::string_view PhysicalFilePath, FTextureSourceData& OutSourceData, std::string& OutError) -> bool;
	ENGINE_API auto DecodeRGBA8(
		std::span<const uint8> EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool;

	// Builds and platform-compresses the complete mip chain used by both 2D and cube textures.
	ENGINE_API auto BuildMipChain(const FTextureSourceData& SourceData, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, std::string& OutError, uint32 MaxResolution = 0,
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average,
		float AlphaCoverageThreshold = 0.5f,
		const FBuildExecutionControl* ExecutionControl = nullptr) -> bool;
}

#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin::TextureBuilder
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

	TEXTUREBUILD_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Builds and platform-compresses the complete mip chain. Cube callers override
	// transparency so every face uses the format selected for the entire cube.
	TEXTUREBUILD_API auto BuildMipChain(std::span<const Image::FImage> SourceMips, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, uint32 MaxResolution = 0,
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average,
		float AlphaCoverageThreshold = 0.5f,
		const FBuildExecutionControl* ExecutionControl = nullptr,
		std::optional<bool> TransparencyOverride = {}) -> FTexture2DBuildResult;

	// Adapts existing cube-face callers to the image recipe.
	TEXTUREBUILD_API auto BuildMipChain(const FTextureSourceData& SourceData, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, uint32 MaxResolution = 0,
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average,
		float AlphaCoverageThreshold = 0.5f,
		const FBuildExecutionControl* ExecutionControl = nullptr,
		std::span<const FTextureSourceData> SuppliedMips = {}) -> FTexture2DBuildResult;
}

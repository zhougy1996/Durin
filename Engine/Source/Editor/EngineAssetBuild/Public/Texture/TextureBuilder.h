#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/Texture2D.h"

namespace Durin::AssetBuild::TextureBuilder
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

	ENGINEASSETBUILD_API auto IsValidUsage(ETextureUsage Usage) -> bool;
	ENGINEASSETBUILD_API auto GetDefaultSRGB(ETextureUsage Usage) -> bool;
	ENGINEASSETBUILD_API auto IsValidCompressionQuality(ETextureCompressionQuality Quality) -> bool;
	ENGINEASSETBUILD_API auto IsValidAlphaMipMode(ETextureAlphaMipMode Mode) -> bool;
	ENGINEASSETBUILD_API auto IsValidAlphaCoverageThreshold(float Threshold) -> bool;
	ENGINEASSETBUILD_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Builds and platform-compresses the complete mip chain used by both 2D and cube textures.
	ENGINEASSETBUILD_API auto BuildMipChain(const FTextureSourceData& SourceData, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, std::string& OutError, uint32 MaxResolution = 0,
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average,
		float AlphaCoverageThreshold = 0.5f,
		const FBuildExecutionControl* ExecutionControl = nullptr) -> bool;
}

#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/TextureBuildOutcome.h"
#include "Texture/VolumeTextureData.h"

namespace Durin::VolumeTextureMipGenerator
{
	// Deterministically builds a complete three-axis box-filtered mip chain.
	TEXTURECOMPRESSOR_API auto GenerateMipChain(
		const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTexturePlatformData& OutPlatformData) -> std::expected<void, FTextureBuildError>;
}

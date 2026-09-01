#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTexture.h"

namespace Durin::VolumeTextureBuilder
{
	// Deterministically builds a complete three-axis box-filtered mip chain.
	TEXTUREBUILD_API auto BuildMipChain(
		const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTexturePlatformData& OutPlatformData,
		std::string& OutError) -> bool;
}

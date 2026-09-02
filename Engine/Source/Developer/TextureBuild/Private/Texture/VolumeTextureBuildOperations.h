#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError,
		bool bPersistDerivedData = true) -> bool;
}

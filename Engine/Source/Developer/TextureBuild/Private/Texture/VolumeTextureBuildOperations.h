#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureBuildInput& Request) -> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError>;
}

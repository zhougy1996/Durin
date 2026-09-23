#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureRecipeBuildRequest& Request) -> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError>;
}

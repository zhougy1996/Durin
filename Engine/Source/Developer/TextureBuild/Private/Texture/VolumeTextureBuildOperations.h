#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuild.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureRecipeBuildRequest& Request) -> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError>;
}

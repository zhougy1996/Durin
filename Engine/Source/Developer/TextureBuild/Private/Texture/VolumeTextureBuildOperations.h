#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureRecipeBuildRequest& Request,
		FVolumeTextureRecipeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}

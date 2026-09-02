#pragma once

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTextureCube(
		const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}

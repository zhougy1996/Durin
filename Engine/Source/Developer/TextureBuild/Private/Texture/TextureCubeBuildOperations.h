#pragma once

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto NormalizeTextureCube(
		const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCube(
		const FTextureCubeRecipeBuildRequest& Request,
		FTextureCubeRecipeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}

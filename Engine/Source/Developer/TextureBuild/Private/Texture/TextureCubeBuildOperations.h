#pragma once

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto NormalizeTextureCube(
		const FTextureCubeBuildRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError>;
	TEXTUREBUILD_API auto BuildTextureCube(
		const FTextureCubeRecipeBuildRequest& Request) -> std::expected<FTextureCubeRecipeBuildProduct, FTextureBuildError>;
}

#pragma once

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto NormalizeTextureCube(
		const FTextureCubeNormalizeRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError>;
	TEXTUREBUILD_API auto BuildTextureCube(
		const FTextureCubeBuildInput& Request) -> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>;
}

#pragma once

#include "CoreMinimal.h"

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto NormalizeTextureCube(
		const FTextureCubeNormalizeRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError>;
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTUREBUILD_API auto BuildTextureCube(
		const FTextureCubeBuildInput& Request) -> std::optional<FTextureCubePlatformData>;
}

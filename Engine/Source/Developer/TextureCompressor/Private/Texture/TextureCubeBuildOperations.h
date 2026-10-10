#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/TextureCubeBuildTypes.h"

namespace Durin
{
	TEXTURECOMPRESSOR_API auto NormalizeTextureCube(
		const FTextureCubeNormalizeRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError>;
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTURECOMPRESSOR_API auto BuildTextureCube(
		const FTextureCubeBuildInput& Request) -> std::optional<FTextureCubePlatformData>;
}

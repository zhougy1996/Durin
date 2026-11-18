#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/TextureCubeValidation.h"

namespace Durin
{
	TEXTURECOMPRESSOR_API auto NormalizeTextureCube(
		const FTextureCubeNormalizeRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureCompressorDiagnostic>;
	// Returns no partial product on failure or cancellation.
	TEXTURECOMPRESSOR_API auto BuildTextureCube(
		const FTextureCubeBuildInput& Request) -> std::expected<FTextureCubePlatformData, std::string>;
}

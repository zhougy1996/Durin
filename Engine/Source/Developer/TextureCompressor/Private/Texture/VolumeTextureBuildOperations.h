#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Returns no partial product on failure or cancellation.
	TEXTURECOMPRESSOR_API auto BuildVolumeTexture(
		const FVolumeTextureBuildInput& Request) -> std::expected<FVolumeTexturePlatformData, std::string>;
}

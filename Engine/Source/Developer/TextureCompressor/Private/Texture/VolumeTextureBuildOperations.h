#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTURECOMPRESSOR_API auto BuildVolumeTexture(
		const FVolumeTextureBuildInput& Request) -> std::optional<FVolumeTexturePlatformData>;
}

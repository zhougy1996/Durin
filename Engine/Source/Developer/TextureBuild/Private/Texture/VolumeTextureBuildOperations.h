#pragma once

#include "CoreMinimal.h"

#include "TextureBuildAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"

namespace Durin
{
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTUREBUILD_API auto BuildVolumeTexture(
		const FVolumeTextureBuildInput& Request) -> std::optional<FVolumeTexturePlatformData>;
}

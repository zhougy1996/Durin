#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Returns no partial product on failure or cancellation.
	TEXTURECOMPRESSOR_API auto BuildTexture2D(
		const FTexture2DBuildInput& Request) -> std::expected<FTexturePlatformData, std::string>;
}

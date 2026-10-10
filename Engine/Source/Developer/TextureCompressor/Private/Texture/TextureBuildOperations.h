#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTURECOMPRESSOR_API auto BuildTexture2D(
		const FTexture2DBuildInput& Request) -> std::optional<FTexture2DBuildOutput>;
}

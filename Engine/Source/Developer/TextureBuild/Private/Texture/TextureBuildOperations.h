#pragma once

#include "CoreMinimal.h"

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Logs failures and returns no partial product; cancellation belongs to the caller.
	TEXTUREBUILD_API auto BuildTexture2D(
		const FTexture2DBuildInput& Request) -> std::optional<FTexture2DBuildOutput>;
}

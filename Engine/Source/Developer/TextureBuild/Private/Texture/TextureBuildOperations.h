#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTexture2D(
		const FTexture2DBuildInput& Request,
		const FTexture2DBuildControl* ExecutionControl = nullptr) -> std::expected<FTexture2DBuildOutput, FTexture2DBuildError>;
}

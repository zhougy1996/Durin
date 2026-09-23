#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTexture2D(
		const FTexture2DRecipeBuildRequest& Request,
		const FTexture2DRecipeExecutionControl* ExecutionControl = nullptr) -> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError>;
}

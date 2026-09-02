#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTexture2D(
		const FTexture2DRecipeBuildRequest& Request,
		FTexture2DRecipeBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DRecipeExecutionControl* ExecutionControl = nullptr) -> bool;
}

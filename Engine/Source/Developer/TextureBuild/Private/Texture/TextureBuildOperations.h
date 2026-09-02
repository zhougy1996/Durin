#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	TEXTUREBUILD_API auto BuildTexture2D(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> bool;
}

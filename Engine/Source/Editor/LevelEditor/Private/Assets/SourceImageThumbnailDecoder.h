#pragma once

#include "HAL/Platform.h"

namespace Durin
{
	// Carries decoded RGBA thumbnail pixels and their dimensions.
	struct FDecodedSourceImageThumbnail
	{
		std::vector<uint8> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bHasTransparency = false;
	};

	auto IsSupportedSourceImageExtension(std::string_view Extension) -> bool;
	auto DecodeSourceImageThumbnail(std::string_view FilePath, uint32 MaximumDimension, FDecodedSourceImageThumbnail& OutThumbnail, std::string& OutError) -> bool;
} // namespace Durin

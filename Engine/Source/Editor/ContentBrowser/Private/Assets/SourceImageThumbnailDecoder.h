#pragma once

#include "HAL/Platform.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Carries decoded RGBA thumbnail pixels and their dimensions.
	struct FDecodedSourceImageThumbnail
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bHasTransparency = false;
	};

	auto IsSupportedSourceImageExtension(std::string_view Extension) -> bool;
	auto DecodeSourceImageThumbnail(std::string_view FilePath, uint32 MaximumDimension, FDecodedSourceImageThumbnail& OutThumbnail, std::string& OutError) -> bool;
} // namespace Durin::Editor::ContentBrowser::Private

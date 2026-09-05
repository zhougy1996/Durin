#pragma once

#include "CoreAPI.h"
#include "HAL/Platform.h"
#include "Image/Image.h"

namespace Durin::Image
{
	// Encodes tightly packed, top-left-origin RGBA8 pixels as a compressed PNG.
	// Failure clears OutEncodedBytes.
	CORE_API auto EncodeRgba8Png(
		FByteView Pixels,
		uint32 Width,
		uint32 Height,
		FByteBuffer& OutEncodedBytes) -> bool;
	CORE_API auto EncodeRgba8Png(
		FImageView Image,
		FByteBuffer& OutEncodedBytes) -> bool;
} // namespace Durin::Image

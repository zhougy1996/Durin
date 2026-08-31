#pragma once

#include "CoreAPI.h"
#include "HAL/Platform.h"

namespace Durin::Image
{
	// Encodes tightly packed, top-left-origin RGBA8 pixels as a compressed PNG.
	// Failure clears OutEncodedBytes.
	CORE_API auto EncodeRgba8Png(
		std::span<const std::byte> Pixels,
		uint32 Width,
		uint32 Height,
		FByteArray& OutEncodedBytes) -> bool;
} // namespace Durin::Image

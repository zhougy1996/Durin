#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	using FByteBuffer = std::vector<std::byte>;
	using FByteView = std::span<const std::byte>;
	using FMutableByteView = std::span<std::byte>;
}

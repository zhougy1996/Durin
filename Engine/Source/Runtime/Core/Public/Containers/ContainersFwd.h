#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace Durin
{
	using FByteBuffer = std::vector<std::byte>;
	using FByteView = std::span<const std::byte>;
	using FMutableByteView = std::span<std::byte>;
}

#pragma once

#include "CoreAPI.h"

namespace Durin::FileTime
{
	// Persistent file timestamps use signed nanoseconds in the platform file-clock domain.
	CORE_API auto ToStableTicks(const std::filesystem::file_time_type& Time) -> int64;
	CORE_API auto FromStableTicks(int64 Ticks) -> std::filesystem::file_time_type;
} // namespace Durin::FileTime

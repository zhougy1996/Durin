#include "Misc/FileTime.h"

namespace Durin::FileTime
{
	auto ToStableTicks(const std::filesystem::file_time_type& Time) -> int64
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Time.time_since_epoch()).count();
	}

	auto FromStableTicks(int64 Ticks) -> std::filesystem::file_time_type
	{
		return std::filesystem::file_time_type{
			std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(Ticks))};
	}
} // namespace Durin::FileTime

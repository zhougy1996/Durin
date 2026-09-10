#include "Misc/Time.h"

namespace Durin
{
	double GStartTime = FTime::Seconds();

	namespace FTime
	{
		auto Nanoseconds() -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		auto Seconds() -> double
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}
} // namespace Durin

#include "Misc/Time.h"

namespace Durin
{
	double GStartTime = FTime::Seconds();

	namespace FTime
	{
		auto Seconds() -> double
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}
} // namespace Durin

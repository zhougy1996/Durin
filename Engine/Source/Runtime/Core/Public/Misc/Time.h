#pragma once

#include "CoreAPI.h"

namespace Durin
{
	namespace FTime
	{
		// Monotonic time with an unspecified epoch; use differences, never wall-clock dates.
		CORE_API auto Seconds() -> double;
		CORE_API auto Nanoseconds() -> uint64;
	};

	// Measures CPU elapsed time into a caller-owned counter, including early return
	// and exception unwinding. Overwrites on Stop/destruction, rounding down.
	// Stop is idempotent; the counter must outlive this thread-confined scope.
	class FScopedMicrosecondTimer final
	{
	public:
		explicit FScopedMicrosecondTimer(uint64& InOutput)
			: Output(InOutput), Started(FTime::Nanoseconds()) {}
		~FScopedMicrosecondTimer() { Stop(); }
		FScopedMicrosecondTimer(const FScopedMicrosecondTimer&) = delete;
		auto operator=(const FScopedMicrosecondTimer&) -> FScopedMicrosecondTimer& = delete;

		auto Stop() -> void
		{
			if (!bStopped)
			{
				Output = (FTime::Nanoseconds() - Started) / 1000;
				bStopped = true;
			}
		}

	private:
		uint64& Output;
		uint64 Started;
		bool bStopped = false;
	};
}
#include "Misc/Time.h"

#include <gtest/gtest.h>
#include <thread>

namespace Durin
{
	TEST(TimeTests, ScopedTimerStopsOnceAndRecordsUnwinding)
	{
		const uint64 Before = FTime::Nanoseconds();
		uint64 Duration = 0;
		uint64 Stopped = 0;
		{
			FScopedMicrosecondTimer Timer(Duration);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Timer.Stop();
			Stopped = Duration;
			EXPECT_GE(Stopped, 1000u);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Timer.Stop();
			EXPECT_EQ(Duration, Stopped);
		}
		EXPECT_EQ(Duration, Stopped);
		EXPECT_GE(FTime::Nanoseconds(), Before);
		Duration = 0;
		try
		{
			FScopedMicrosecondTimer Timer(Duration);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			throw std::runtime_error("timer unwind");
		}
		catch (const std::runtime_error&) {}
		EXPECT_GE(Duration, 1000u);
	}
}

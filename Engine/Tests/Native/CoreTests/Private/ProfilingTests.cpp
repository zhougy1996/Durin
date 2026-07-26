#include "gtest/gtest.h"

#include "Profiling/Profiling.h"

namespace Durin
{
	TEST(FProfilingTests, DisabledMacrosDoNotEvaluateArguments)
	{
		static_assert(DURIN_WITH_TRACY == 0);
		int EvaluationCount = 0;

		DURIN_PROFILE_CPU_ZONE_NAMED((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_THREAD((++EvaluationCount, "Unexpected"));

		EXPECT_EQ(EvaluationCount, 0);
	}
}

// Compile the disabled public-header contract independently of the runtime's
// Tracy setting. CMake excludes this source from shared PCH reuse.
#undef DURIN_WITH_TRACY
#define DURIN_WITH_TRACY 0
#include "CoreMinimal.h"
#include "Profiling/Profiling.h"

#include "gtest/gtest.h"

namespace Durin
{
	TEST(FDisabledProfilingTests, MacrosDoNotEvaluateArguments)
	{
		static_assert(DURIN_WITH_TRACY == 0);
		int EvaluationCount = 0;

		DURIN_PROFILE_CPU_ZONE();
		DURIN_PROFILE_CPU_ZONE_NAMED((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_CPU_ZONE_TEXT((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_STARTUP_FIRST_PRESENT();
		DURIN_PROFILE_FRAME_MARK();
		DURIN_PROFILE_THREAD((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_PROGRAM_IDENTITY(
			(++EvaluationCount, "Unexpected"),
			(++EvaluationCount, "Unexpected"),
			++EvaluationCount
		);

		EXPECT_EQ(EvaluationCount, 0);
	}
}

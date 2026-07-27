#include "gtest/gtest.h"

#include "Profiling/Profiling.h"

namespace Durin
{
	TEST(FProfilingTests, FormatsProgramIdentity)
	{
		EXPECT_EQ(
			Profiling::FormatProgramIdentity("DurinEditor", "Sandbox", 1234),
			"DurinEditor | Sandbox | PID 1234"
		);
	}

	TEST(FProfilingTests, FormatsFallbackProgramIdentity)
	{
		EXPECT_EQ(Profiling::FormatProgramIdentity({}, {}, 7), "Durin | No Project | PID 7");
	}

	TEST(FProfilingTests, OwnsStoredProgramIdentity)
	{
		std::string ProjectName = "Sandbox";
		const std::string_view FirstIdentity = Profiling::SetProgramIdentity("DurinGame", ProjectName, 42);
		ProjectName = "Changed";
		const std::string_view SecondIdentity = Profiling::SetProgramIdentity(
			"DurinEditor",
			"A Project Name Long Enough To Require Different String Storage",
			43
		);

		EXPECT_EQ(FirstIdentity, "DurinGame | Sandbox | PID 42");
		EXPECT_EQ(
			SecondIdentity,
			"DurinEditor | A Project Name Long Enough To Require Different String Storage | PID 43"
		);
		EXPECT_EQ(Profiling::GetProgramIdentity(), SecondIdentity);
	}

	TEST(FProfilingTests, DisabledMacrosDoNotEvaluateArguments)
	{
		static_assert(DURIN_WITH_TRACY == 0);
		int EvaluationCount = 0;

		DURIN_PROFILE_CPU_ZONE_NAMED((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_THREAD((++EvaluationCount, "Unexpected"));
		DURIN_PROFILE_PROGRAM_IDENTITY(
			(++EvaluationCount, "Unexpected"),
			(++EvaluationCount, "Unexpected"),
			++EvaluationCount
		);

		EXPECT_EQ(EvaluationCount, 0);
	}
}

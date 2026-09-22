#include "gtest/gtest.h"

#include "Profiling/Profiling.h"

namespace Durin
{
	TEST(FProfilingTests, FormatsExplicitAndFallbackProgramIdentities)
	{
		EXPECT_EQ(
			Profiling::FormatProgramIdentity("DurinEditor", "Sandbox", 1234),
			"DurinEditor | Sandbox | PID 1234"
		);
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

	TEST(FProfilingTests, FormatsExplicitPortOverrideDiagnostic)
	{
		const std::string Diagnostic = Profiling::FormatPortOverrideDiagnostic("8099");

		EXPECT_NE(Diagnostic.find("TRACY_PORT=8099"), std::string::npos);
		EXPECT_NE(Diagnostic.find("disables Tracy's automatic 8086-8105 port search"), std::string::npos);
		EXPECT_NE(Diagnostic.find("second process"), std::string::npos);
	}

#if DURIN_WITH_TRACY
	TEST(FProfilingTests, EnabledIdentityMacroEvaluatesArgumentsOnce)
	{
		int EvaluationCount = 0;
		DURIN_PROFILE_PROGRAM_IDENTITY(
			(++EvaluationCount, "DurinEditor"),
			(++EvaluationCount, "ProfilingTest"),
			(++EvaluationCount, 42)
		);
		EXPECT_EQ(EvaluationCount, 3);
		EXPECT_EQ(Profiling::GetProgramIdentity(), "DurinEditor | ProfilingTest | PID 42");
	}
#endif

	TEST(FProfilingTests, ZoneAnnotationsAcceptOwnedAndBorrowedText)
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Tests.Profiling.Annotations");
		const std::string Owned = "owned annotation";
		DURIN_PROFILE_CPU_ZONE_TEXT(Owned);
		DURIN_PROFILE_CPU_ZONE_TEXT(std::string_view(Owned));
		DURIN_PROFILE_CPU_ZONE_TEXT(std::string("temporary annotation"));
		DURIN_PROFILE_CPU_ZONE_TEXT("literal annotation");
		int EvaluationCount = 0;
		DURIN_PROFILE_CPU_ZONE_TEXT((++EvaluationCount, Owned));
		// An on-demand zone may be inactive; an active annotation evaluates once.
		EXPECT_LE(EvaluationCount, 1);
	}

	TEST(FProfilingTests, StartupMilestonesRetainTheFirstObservation)
	{
		EXPECT_TRUE(Profiling::RecordStartupMilestone(
			Profiling::EStartupMilestone::DefaultDocumentBegin));
		const double FirstObservation = Profiling::GetStartupMilestoneMilliseconds(
			Profiling::EStartupMilestone::DefaultDocumentBegin);

		EXPECT_FALSE(Profiling::RecordStartupMilestone(
			Profiling::EStartupMilestone::DefaultDocumentBegin));
		EXPECT_DOUBLE_EQ(
			Profiling::GetStartupMilestoneMilliseconds(
				Profiling::EStartupMilestone::DefaultDocumentBegin),
			FirstObservation);
		EXPECT_GE(FirstObservation, 0.0);
		EXPECT_FALSE(Profiling::TryLogStartupTimingSummary());
	}
}

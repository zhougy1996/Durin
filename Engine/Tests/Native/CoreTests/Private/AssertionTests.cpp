#include <gtest/gtest.h>

#include "AssertionPublicHeaderFixture.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Build.h"

namespace
{
	auto Increment(int& Value) -> int
	{
		return ++Value;
	}
}

TEST(FAssertionTests, ConfigurationEvaluationCountsMatchContract)
{
	int CheckCount = 0;
	int CheckFormatCount = 0;
	int VerifyCount = 0;
	int VerifyFormatCount = 0;
	int RequireCount = 0;
	int RequireFormatCount = 0;
	int SlowCount = 0;
	int SlowFormatCount = 0;

	check(Increment(CheckCount) == 1);
	checkf(Increment(CheckFormatCount) == 1, "{}", Increment(CheckFormatCount));
	verify(Increment(VerifyCount) == 1);
	verifyf(Increment(VerifyFormatCount) == 1, "{}", Increment(VerifyFormatCount));
	require(Increment(RequireCount) == 1);
	requiref(Increment(RequireFormatCount) == 1, "{}", Increment(RequireFormatCount));
	checkSlow(Increment(SlowCount) == 1);
	checkfSlow(Increment(SlowFormatCount) == 1, "{}", Increment(SlowFormatCount));

	EXPECT_EQ(CheckCount, DO_CHECK ? 1 : 0);
	EXPECT_EQ(CheckFormatCount, DO_CHECK ? 1 : 0);
	EXPECT_EQ(VerifyCount, 1);
	EXPECT_EQ(VerifyFormatCount, 1);
	EXPECT_EQ(RequireCount, 1);
	EXPECT_EQ(RequireFormatCount, 1);
	EXPECT_EQ(SlowCount, DURIN_BUILD_DEBUG ? 1 : 0);
	EXPECT_EQ(SlowFormatCount, DURIN_BUILD_DEBUG ? 1 : 0);

	ExerciseAssertionPublicHeader(true);
}

TEST(FAssertionTests, DisabledFormattedArgumentsAreNotEvaluated)
{
	int CheckConditionCount = 0;
	int CheckFormatCount = 0;
	int VerifyConditionCount = 0;
	int VerifyFormatCount = 0;
	int RequireFormatCount = 0;
	int SlowConditionCount = 0;
	int SlowFormatCount = 0;

#if !DO_CHECK
	check(Increment(CheckConditionCount) == 1);
	checkf(false, "{}", Increment(CheckFormatCount));
	verifyf(
		Increment(VerifyConditionCount) == 0,
		"{}",
		Increment(VerifyFormatCount));
#endif
	requiref(true, "{}", Increment(RequireFormatCount));

#if !DURIN_BUILD_DEBUG
	checkSlow(Increment(SlowConditionCount) == 1);
	checkfSlow(false, "{}", Increment(SlowFormatCount));
#endif

	EXPECT_EQ(CheckConditionCount, 0);
	EXPECT_EQ(CheckFormatCount, 0);
	EXPECT_EQ(VerifyConditionCount, DO_CHECK ? 0 : 1);
	EXPECT_EQ(VerifyFormatCount, 0);
	EXPECT_EQ(RequireFormatCount, 0);
	EXPECT_EQ(SlowConditionCount, 0);
	EXPECT_EQ(SlowFormatCount, 0);
}

TEST(FAssertionTests, ExpansionsAreStatementSafe)
{
	bool bElseReached = false;
	if (true)
		check(true);
	else
		bElseReached = true;

	EXPECT_FALSE(bElseReached);

	if (true)
		require(true);
	else
		bElseReached = true;

	EXPECT_FALSE(bElseReached);
}

#if DO_CHECK
TEST(FAssertionTests, EnabledFailuresReportExpressionContextAndSource)
{
	EXPECT_DEATH(
		checkf(false, "Assertion fixture context {}", 42),
		"Assertion fixture context 42");
}
#endif

TEST(FAssertionTests, RequiredFailuresReportAndTerminateInEveryConfiguration)
{
	EXPECT_DEATH(
		requiref(false, "Required fixture context {}", 42),
		"Required fixture context 42");
}

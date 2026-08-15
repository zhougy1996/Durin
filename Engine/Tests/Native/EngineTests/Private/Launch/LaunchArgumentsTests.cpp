#include "Runtime/Launch/Private/LaunchArguments.h"

#include <gtest/gtest.h>

namespace
{
	auto Parse(std::initializer_list<std::string_view> Arguments)
		-> Durin::FLaunchArgumentResult
	{
		return Durin::ParseLaunchArguments(Arguments);
	}

	void ExpectRejected(std::initializer_list<std::string_view> Arguments, std::string_view Option)
	{
		const Durin::FLaunchArgumentResult Result = Parse(Arguments);
		ASSERT_FALSE(Result);
		ASSERT_TRUE(Result.Error);
		EXPECT_EQ(Result.Error->ExitCode, 2);
		EXPECT_EQ(Result.Error->Option, Option);
		EXPECT_FALSE(Result.Error->Message.empty());
	}
}

TEST(FLaunchArgumentsTests, ParsesOwnedRequestDomainsAndNormalizesProjectForms)
{
	auto Equals = Parse({"--wait-for-process=42", "--project=Sandbox/Sandbox.dproject",
		"--hidden-window", "--exit-after-ticks=7"});
	ASSERT_TRUE(Equals);
	EXPECT_EQ(Equals.Request->ProcessCoordination.WaitForProcessId, 42u);
	EXPECT_EQ(Equals.Request->Host.ProjectFile, "Sandbox/Sandbox.dproject");
	EXPECT_TRUE(Equals.Request->Host.bSuppressWindowDisplay);
	EXPECT_EQ(Equals.Request->Automation.ExitAfterTicks, 7u);

	auto Separate = Parse({"--project", "Sandbox/Sandbox.dproject"});
	ASSERT_TRUE(Separate);
	EXPECT_EQ(Separate.Request->Host.ProjectFile, Equals.Request->Host.ProjectFile);
}

TEST(FLaunchArgumentsTests, PreservesRepeatedStartupCommandArgumentOrder)
{
	auto Result = Parse({"--startup-command=Cook", "--startup-command-arg=first",
		"--startup-command-arg=", "--startup-command-arg=third"});
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result.Request->Automation.StartupCommand.Name, "Cook");
	EXPECT_EQ(Result.Request->Automation.StartupCommand.Arguments,
		(std::vector<std::string>{"first", "", "third"}));
}

TEST(FLaunchArgumentsTests, RejectsUnknownDuplicateAndMixedProjectForms)
{
	ExpectRejected({"--unknown"}, "--unknown");
	ExpectRejected({"--engine-asset-service-lifecycle-smoke"},
		"--engine-asset-service-lifecycle-smoke");
	ExpectRejected({"--hidden-window", "--hidden-window"}, "--hidden-window");
	ExpectRejected({"--project=One", "--project", "Two"}, "--project");
	ExpectRejected({"--exit-after-ticks=1", "--exit-after-ticks=2"}, "--exit-after-ticks");
}

TEST(FLaunchArgumentsTests, RejectsEmptyMalformedZeroOverflowAndTrailingNumericValues)
{
	ExpectRejected({"--project="}, "--project");
	ExpectRejected({"--project"}, "--project");
	ExpectRejected({"--project", "--hidden-window"}, "--project");
	ExpectRejected({"--wait-for-process=0"}, "--wait-for-process");
	ExpectRejected({"--wait-for-process=abc"}, "--wait-for-process");
	ExpectRejected({"--exit-after-ticks=12x"}, "--exit-after-ticks");
	ExpectRejected({"--exit-after-ticks=18446744073709551616"}, "--exit-after-ticks");
}

TEST(FLaunchArgumentsTests, RejectsStartupCommandCompanionAndConflictErrors)
{
	ExpectRejected({"--startup-command="}, "--startup-command");
	ExpectRejected({"--startup-command-arg=value"}, "--startup-command-arg");
	ExpectRejected({"--startup-command=Cook", "--exit-after-ticks=1"}, "--startup-command");
	ExpectRejected({"--startup-command=Cook", "--project-browser"}, "--startup-command");
	ExpectRejected({"--startup-command=Cook", "--task-scheduler-lifecycle-smoke"}, "--startup-command");
}

TEST(FLaunchArgumentsTests, ParsesTypedCrashPhasesAndRejectsInvalidCompanions)
{
	auto Result = Parse({"--native-crash-fixture=access-read",
		"--native-crash-at=object-collection", "--native-crash-saved=D:/Saved"});
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result.Request->Diagnostics.NativeCrashPhase,
		Durin::ENativeCrashPhase::ObjectCollection);
	ExpectRejected({"--native-crash-at=nowhere"}, "--native-crash-at");
	ExpectRejected({"--native-crash-at=running"}, "--native-crash-at");
	ExpectRejected({"--native-crash-log-gap"}, "--native-crash-log-gap");
	ExpectRejected({"--native-crash-fixture=unknown"}, "--native-crash-fixture");
}

TEST(FLaunchArgumentsTests, RepeatedCallsRetainNoParserState)
{
	EXPECT_TRUE(Parse({"--project=One"}));
	EXPECT_TRUE(Parse({"--project=Two"}));
	EXPECT_TRUE(Parse({}));
}

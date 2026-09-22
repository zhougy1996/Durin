#include <gtest/gtest.h>

#include "Misc/StartupCommand.h"

TEST(FStartupCommandTests, DispatchesOneOpaqueCommandAfterHandlerRegistration)
{
	std::vector<std::string> Received;
	const uint64 Handle = Durin::RegisterStartupCommandHandler(
		"test.opaque", [&Received](std::span<const std::string> Arguments) {
			Received.assign(Arguments.begin(), Arguments.end());
			return 17;
		});
	ASSERT_NE(Handle, 0u);
	const auto ConfigureStartupCommandResult = Durin::ConfigureStartupCommand("test.opaque", {"--alpha=one", "--beta=two"});
	ASSERT_TRUE(ConfigureStartupCommandResult.has_value()) << ConfigureStartupCommandResult.error().ToString();
	const auto Result = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Result.has_value());
	ASSERT_TRUE(Result->has_value());
	EXPECT_EQ(**Result, 17);
	EXPECT_EQ(Received,
		(std::vector<std::string>{"--alpha=one", "--beta=two"}));
	Durin::UnregisterStartupCommandHandler(Handle);
	const auto Idle = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Idle);
	EXPECT_FALSE(Idle->has_value());
}

TEST(FStartupCommandTests, RejectsASecondPendingCommand)
{
	const auto ConfigureStartupCommandResult2 = Durin::ConfigureStartupCommand("test.first", {});
	ASSERT_TRUE(ConfigureStartupCommandResult2.has_value());
	const auto ConfigureStartupCommandResult3 = Durin::ConfigureStartupCommand("test.second", {});
	ASSERT_FALSE(ConfigureStartupCommandResult3);
	EXPECT_EQ(ConfigureStartupCommandResult3.error().Code, Durin::EStartupCommandError::AlreadyPending);
	const auto Pending = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Pending);
	EXPECT_FALSE(Pending->has_value());
	EXPECT_TRUE(Durin::HasPendingStartupCommand());
	const auto Result = Durin::DispatchStartupCommand(true);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, Durin::EStartupCommandError::MissingHandler);
	EXPECT_FALSE(Durin::HasPendingStartupCommand());
}

TEST(FStartupCommandTests, RejectsEmptyNameWithoutQueuingCommand)
{
	const auto Result = Durin::ConfigureStartupCommand({}, {});
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, Durin::EStartupCommandError::InvalidName);
	EXPECT_FALSE(Durin::HasPendingStartupCommand());
}

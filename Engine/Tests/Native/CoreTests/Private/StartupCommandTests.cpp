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
	std::string Error;
	const auto ConfigureStartupCommandResult = Durin::ConfigureStartupCommand("test.opaque", {"--alpha=one", "--beta=two"});
	Error = ConfigureStartupCommandResult ? std::string{} : ConfigureStartupCommandResult.error().ToString();
	ASSERT_TRUE(ConfigureStartupCommandResult.has_value()) << Error;
	const auto Result = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Result.has_value());
	ASSERT_TRUE(Result->has_value());
	EXPECT_EQ(**Result, 17);
	EXPECT_TRUE(Error.empty());
	EXPECT_EQ(Received,
		(std::vector<std::string>{"--alpha=one", "--beta=two"}));
	Durin::UnregisterStartupCommandHandler(Handle);
	const auto Idle = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Idle);
	EXPECT_FALSE(Idle->has_value());
}

TEST(FStartupCommandTests, RejectsASecondPendingCommand)
{
	std::string Error;
	const auto ConfigureStartupCommandResult2 = Durin::ConfigureStartupCommand("test.first", {});
	Error = ConfigureStartupCommandResult2 ? std::string{} : ConfigureStartupCommandResult2.error().ToString();
	ASSERT_TRUE(ConfigureStartupCommandResult2.has_value());
	const auto ConfigureStartupCommandResult3 = Durin::ConfigureStartupCommand("test.second", {});
	Error = ConfigureStartupCommandResult3 ? std::string{} : ConfigureStartupCommandResult3.error().ToString();
	EXPECT_FALSE(ConfigureStartupCommandResult3.has_value());
	ASSERT_FALSE(ConfigureStartupCommandResult3);
	EXPECT_EQ(ConfigureStartupCommandResult3.error().Code, Durin::EStartupCommandError::AlreadyPending);
	EXPECT_FALSE(Error.empty());
	const auto Pending = Durin::DispatchStartupCommand();
	ASSERT_TRUE(Pending);
	EXPECT_FALSE(Pending->has_value());
	EXPECT_TRUE(Durin::HasPendingStartupCommand());
	const auto Result = Durin::DispatchStartupCommand(true);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, Durin::EStartupCommandError::MissingHandler);
	EXPECT_FALSE(Result.error().ToString().empty());
	EXPECT_FALSE(Durin::HasPendingStartupCommand());
}

TEST(FStartupCommandTests, RejectsEmptyNameWithoutQueuingCommand)
{
	const auto Result = Durin::ConfigureStartupCommand({}, {});
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, Durin::EStartupCommandError::InvalidName);
	EXPECT_FALSE(Durin::HasPendingStartupCommand());
}

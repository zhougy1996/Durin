#include <gtest/gtest.h>

#include "Misc/StartupCommand.h"
#include "Modules/ModuleTestSupport.h"

TEST(FStartupCommandTests, DispatchesOneOpaqueCommandAfterHandlerRegistration)
{
	static Durin::FModuleTestOwner Context("StartupCommandTests");
	static auto Registration = Context.CreateOwnedCallbackRegistration(
		"Tests.StartupCommands");
	std::vector<std::string> Received;
	const Durin::uint64 Handle = Durin::RegisterStartupCommandHandler(
		"test.opaque", [&Received](std::span<const std::string> Arguments) {
			Received.assign(Arguments.begin(), Arguments.end());
			return 17;
		}, Registration.GetGate());
	ASSERT_NE(Handle, 0u);
	std::string Error;
	ASSERT_TRUE(Durin::ConfigureStartupCommand(
		"test.opaque", {"--alpha=one", "--beta=two"}, &Error)) << Error;
	const std::optional<int> Result = Durin::DispatchStartupCommand(&Error);
	ASSERT_TRUE(Result.has_value());
	EXPECT_EQ(*Result, 17);
	EXPECT_TRUE(Error.empty());
	EXPECT_EQ(Received,
		(std::vector<std::string>{"--alpha=one", "--beta=two"}));
	Durin::UnregisterStartupCommandHandler(Handle);
	EXPECT_FALSE(Durin::DispatchStartupCommand(&Error).has_value());
}

TEST(FStartupCommandTests, RejectsASecondPendingCommand)
{
	std::string Error;
	ASSERT_TRUE(Durin::ConfigureStartupCommand("test.first", {}, &Error));
	EXPECT_FALSE(Durin::ConfigureStartupCommand("test.second", {}, &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Durin::DispatchStartupCommand(&Error).has_value());
	EXPECT_TRUE(Durin::HasPendingStartupCommand());
	const std::optional<int> Result = Durin::DispatchStartupCommand(&Error, true);
	ASSERT_TRUE(Result.has_value());
	EXPECT_EQ(*Result, 2);
	EXPECT_FALSE(Error.empty());
}

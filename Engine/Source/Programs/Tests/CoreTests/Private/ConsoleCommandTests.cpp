#include "Console/ConsoleCommand.h"
#include "gtest/gtest.h"

namespace Durin
{
	TEST(FConsoleCommandTests, ParsesArgumentsQuotesAndEscapes)
	{
		FConsoleCommandRegistry Registry;
		std::vector<std::string> Captured;
		ASSERT_NE(Registry.RegisterCommand({"echo", "", "", [&Captured](std::span<const std::string> Args) {
			Captured.assign(Args.begin(), Args.end());
			return FConsoleCommandResult::Success();
		}}), 0);
		EXPECT_TRUE(Registry.Execute(R"(ECHO plain "two words" 'three words' escaped\ value "a\"b")").bSuccess);
		EXPECT_EQ(Captured, (std::vector<std::string>{"plain", "two words", "three words", "escaped value", "a\"b"}));
	}

	TEST(FConsoleCommandTests, ReportsInvalidAndUnknownCommands)
	{
		FConsoleCommandRegistry Registry;
		EXPECT_TRUE(Registry.Execute("   ").bSuccess);
		EXPECT_FALSE(Registry.Execute("missing").bSuccess);
		EXPECT_FALSE(Registry.Execute("help 'unfinished").bSuccess);
		EXPECT_FALSE(Registry.Execute("help trailing\\").bSuccess);
	}

	TEST(FConsoleCommandTests, RejectsDuplicatesAndSupportsUnregister)
	{
		FConsoleCommandRegistry Registry;
		const auto Callback = [](std::span<const std::string>) { return FConsoleCommandResult::Success(); };
		const FConsoleCommandHandle Handle = Registry.RegisterCommand({"Test", "", "", Callback});
		ASSERT_NE(Handle, 0);
		EXPECT_EQ(Registry.RegisterCommand({"test", "", "", Callback}), 0);
		EXPECT_TRUE(Registry.Execute("TEST").bSuccess);
		Registry.UnregisterCommand(Handle);
		EXPECT_FALSE(Registry.Execute("test").bSuccess);
	}

	TEST(FConsoleCommandTests, CompletesAndProvidesHelp)
	{
		FConsoleCommandRegistry Registry;
		const auto Callback = [](std::span<const std::string>) { return FConsoleCommandResult::Failure("expected failure"); };
		Registry.RegisterCommand({"hello", "Greets.", "hello", Callback});
		Registry.RegisterCommand({"helm", "Steers.", "helm", Callback});
		EXPECT_EQ(Registry.FindCompletions("HE"), (std::vector<std::string>{"hello", "helm", "help"}));
		EXPECT_TRUE(Registry.Execute("help").bSuccess);
		EXPECT_NE(Registry.Execute("help hello").Message.find("Usage: hello"), std::string::npos);
		EXPECT_FALSE(Registry.Execute("hello").bSuccess);
		EXPECT_FALSE(Registry.Execute("help one two").bSuccess);
	}
} // namespace Durin

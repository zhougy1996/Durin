#include "Console/ConsoleCommand.h"
#include "Modules/ModuleTestContext.h"
#include "gtest/gtest.h"

namespace Durin
{
	TEST(FConsoleCommandTests, ParsesValidArgumentsAndRejectsMalformedOrUnknownInput)
	{
		FConsoleCommandRegistry Registry;
		std::vector<std::string> Captured;
		ASSERT_NE(Registry.RegisterCommand({"echo", "", "", [&Captured](std::span<const std::string> Args) {
			Captured.assign(Args.begin(), Args.end());
			return FConsoleCommandResult::Success();
		}}), 0);
		EXPECT_TRUE(Registry.Execute(R"(ECHO plain "two words" 'three words' escaped\ value "a\"b")").bSuccess);
		EXPECT_EQ(Captured, (std::vector<std::string>{"plain", "two words", "three words", "escaped value", "a\"b"}));
		EXPECT_TRUE(Registry.Execute("   ").bSuccess);
		EXPECT_FALSE(Registry.Execute("missing").bSuccess);
		EXPECT_FALSE(Registry.Execute("help 'unfinished").bSuccess);
		EXPECT_FALSE(Registry.Execute("help trailing\\").bSuccess);
	}

	TEST(FConsoleCommandTests, ManagesRegistrationCompletionHelpAndUnregistration)
	{
		FConsoleCommandRegistry Registry;
		const auto Callback = [](std::span<const std::string>) { return FConsoleCommandResult::Success(); };
		const FConsoleCommandHandle Handle = Registry.RegisterCommand({"Test", "", "", Callback});
		ASSERT_NE(Handle, 0);
		EXPECT_EQ(Registry.RegisterCommand({"test", "", "", Callback}), 0);
		EXPECT_TRUE(Registry.Execute("TEST").bSuccess);
		const auto FailureCallback = [](std::span<const std::string>) {
			return FConsoleCommandResult::Failure("expected failure");
		};
		Registry.RegisterCommand({"hello", "Greets.", "hello", FailureCallback});
		Registry.RegisterCommand({"helm", "Steers.", "helm", FailureCallback});
		EXPECT_EQ(Registry.FindCompletions("HE"), (std::vector<std::string>{"hello", "helm", "help"}));
		EXPECT_TRUE(Registry.Execute("help").bSuccess);
		EXPECT_NE(Registry.Execute("help hello").Message.find("Usage: hello"), std::string::npos);
		EXPECT_FALSE(Registry.Execute("hello").bSuccess);
		EXPECT_FALSE(Registry.Execute("help one two").bSuccess);

		Registry.UnregisterCommand(Handle);
		EXPECT_FALSE(Registry.Execute("test").bSuccess);
	}

	TEST(FConsoleCommandTests, OwnerRetirementRejectsDispatchAndAuditsStoredCallable)
	{
		auto Context = FModuleTestContextFactory::CreateStartupContext(
			"ConsoleCommandTests.Owner");
		auto Owner = Context.CreateOwnedCallbackRegistration("Core.ConsoleCommands");
		FConsoleCommandRegistry Registry;
		auto Capture = std::make_shared<int>(7);
		const std::weak_ptr<int> WeakCapture = Capture;
		const FConsoleCommandHandle Handle = Registry.RegisterCommand({
			"owned", "Owned command.", "owned",
			[Capture](std::span<const std::string>) {
				return FConsoleCommandResult::Success(std::to_string(*Capture));
			}}, Owner.GetGate());
		ASSERT_NE(Handle, 0u);
		Capture.reset();
		const auto Snapshot = Owner.Retire();
		EXPECT_EQ(Snapshot.RetainedResourceCount, 1u);
		EXPECT_FALSE(Registry.Execute("owned").bSuccess);
		const auto Commands = Registry.GetCommands();
		const auto It = std::ranges::find(Commands, std::string("owned"),
			&FConsoleCommandDesc::Name);
		ASSERT_NE(It, Commands.end());
		EXPECT_FALSE(It->Execute);

		Registry.UnregisterCommand(Handle);
		EXPECT_TRUE(WeakCapture.expired());
		EXPECT_TRUE(Owner.Reset(std::chrono::milliseconds(0)).Succeeded());
	}
} // namespace Durin

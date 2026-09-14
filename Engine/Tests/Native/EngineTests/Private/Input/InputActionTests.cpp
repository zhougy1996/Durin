#include "Input/InputActions.h"
#include "Input/GameInputState.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace Durin
{
	struct FGameInputStateTestAccess
	{
		static auto Enable(FGameInputState& Input) -> void { Input.SetEnabled(true); Input.SetFocused(true); }
		static auto Focus(FGameInputState& Input, bool Focused) -> void { Input.SetFocused(Focused); }
		static auto Key(FGameInputState& Input, EKey Key, bool Down, bool Repeat = false) -> void { Input.SetKey(Key, Down, Repeat); }
		static auto Mouse(FGameInputState& Input, EMouseButton Button, bool Down) -> void { Input.SetMouseButton(Button, Down); }
		static auto Move(FGameInputState& Input, FVector2d Position) -> void { Input.SetMousePosition(Position); }
		static auto Wheel(FGameInputState& Input, double Delta) -> void { Input.AddMouseWheel(Delta); }
		static auto Finish(FGameInputState& Input) -> void { Input.FinishGameTick(); }
		static auto Block(FGameInputState& Input, bool Keyboard, bool Mouse) -> void { Input.bKeyboardBlocked = Keyboard; Input.bMouseBlocked = Mouse; }
	};
}

namespace
{
	using namespace Durin;
	using FAccess = FGameInputStateTestAccess;

	class FInputActionTests : public ::testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			FAccess::Enable(Input);
			ASSERT_TRUE(Actions.DefineAction("Move", EInputActionType::Axis2D));
			ASSERT_TRUE(Actions.DefineAction("Look", EInputActionType::Axis2D));
			ASSERT_TRUE(Actions.DefineAction("Jump", EInputActionType::Button));
			ASSERT_TRUE(Actions.DefineAction("Interact", EInputActionType::Button));
			ASSERT_TRUE(Actions.DefineAction("Zoom", EInputActionType::Axis1D));
			FInputMappingContext Game;
			Game.Name = "Game";
			Game.Bindings = {
				{"Forward", "Move", FInputSource::Key(EKey::W), {0.0, 1.0}},
				{"Back", "Move", FInputSource::Key(EKey::S), {0.0, -1.0}},
				{"Jump", "Jump", FInputSource::Key(EKey::Space)},
				{"JumpAlternate", "Jump", FInputSource::Key(EKey::J)},
				{"Interact", "Interact", FInputSource::Mouse(EMouseButton::Left)},
				{"LookX", "Look", {EInputSourceKind::MouseX}, {1.0, 0.0}},
				{"LookY", "Look", {EInputSourceKind::MouseY}, {0.0, 1.0}},
				{"Zoom", "Zoom", {EInputSourceKind::MouseWheel}, {2.0, 0.0}}};
			ASSERT_TRUE(Actions.AddContext(std::move(Game)));
		}
		auto Sample() -> const FInputActionSnapshot& { return Actions.Evaluate(Input); }
		auto Next() -> void { FAccess::Finish(Input); }
		FGameInputState Input;
		FInputActionEvaluator Actions;
	};
}

TEST_F(FInputActionTests, SameFrameTapAndMultipleBindingsPreserveAggregateEdges)
{
	FAccess::Key(Input, EKey::Space, true);
	FAccess::Key(Input, EKey::Space, false);
	const auto Tap = Sample().Get("Jump");
	EXPECT_TRUE(Tap.bStarted);
	EXPECT_TRUE(Tap.bCompleted);
	EXPECT_FALSE(Tap.bActive);
	EXPECT_FALSE(Tap.bCancelled);
	EXPECT_TRUE(Sample().Get("Jump").bStarted); // Immutable repeated read.
	Next();
	EXPECT_FALSE(Sample().Get("Jump").bStarted);
	Next();
	FAccess::Key(Input, EKey::Space, true);
	Sample();
	Next();
	FAccess::Key(Input, EKey::J, true);
	FAccess::Key(Input, EKey::Space, false);
	const auto Overlap = Sample().Get("Jump");
	EXPECT_TRUE(Overlap.bActive);
	EXPECT_FALSE(Overlap.bStarted);
	EXPECT_FALSE(Overlap.bCompleted);
	Next();
	FAccess::Key(Input, EKey::J, false);
	EXPECT_TRUE(Sample().Get("Jump").bCompleted);
}

TEST_F(FInputActionTests, AxesMouseAndWheelAreSingleFrameValues)
{
	FAccess::Key(Input, EKey::W, true);
	FAccess::Key(Input, EKey::S, true);
	FAccess::Move(Input, {10.0, 20.0});
	FAccess::Move(Input, {14.0, 17.0});
	FAccess::Wheel(Input, 2.0);
	FAccess::Mouse(Input, EMouseButton::Left, true);
	FAccess::Mouse(Input, EMouseButton::Left, false);
	EXPECT_EQ(Sample().Get("Move").Value, FVector2d(0.0));
	EXPECT_EQ(Sample().Get("Look").Value, FVector2d(4.0, -3.0));
	EXPECT_EQ(Sample().Get("Zoom").Value, FVector2d(4.0, 0.0));
	EXPECT_TRUE(Sample().Get("Interact").bStarted);
	EXPECT_TRUE(Sample().Get("Interact").bCompleted);
	Next();
	EXPECT_EQ(Sample().Get("Look").Value, FVector2d(0.0));
	EXPECT_EQ(Sample().Get("Zoom").Value, FVector2d(0.0));
	EXPECT_FALSE(Sample().Get("Interact").bStarted);
}

TEST_F(FInputActionTests, ToolConsumesPhysicalSourceAndEqualPriorityIsStable)
{
	ASSERT_TRUE(Actions.DefineAction("Tool", EInputActionType::Button));
	FInputMappingContext Tool;
	Tool.Name = "EditorTool";
	Tool.Priority = 100;
	Tool.Bindings = {{"Command", "Tool", FInputSource::Key(EKey::W)}};
	ASSERT_TRUE(Actions.AddContext(Tool));
	Tool.Name = "OtherTool";
	Tool.Bindings.front().Action = "Interact";
	ASSERT_TRUE(Actions.AddContext(Tool));
	FAccess::Key(Input, EKey::W, true);
	EXPECT_TRUE(Sample().Get("Tool").bStarted);
	EXPECT_FALSE(Sample().Get("Interact").bStarted);
	EXPECT_FALSE(Sample().Get("Move").bActive);
	ASSERT_TRUE(Actions.RemoveContext("EditorTool"));
	EXPECT_TRUE(Actions.GetSnapshot().Get("Tool").bCancelled);
	Next();
	EXPECT_FALSE(Sample().Get("Interact").bActive);
	FAccess::Key(Input, EKey::W, false);
	Next();
	FAccess::Key(Input, EKey::W, true);
	EXPECT_TRUE(Sample().Get("Interact").bStarted);
}

TEST_F(FInputActionTests, NonConsumingContextAllowsLowerAction)
{
	FInputMappingContext Observer;
	Observer.Name = "Observer";
	Observer.Priority = 1;
	Observer.Bindings = {{"Observe", "Interact", FInputSource::Key(EKey::W), {1.0, 0.0}, false}};
	ASSERT_TRUE(Actions.AddContext(Observer));
	FAccess::Key(Input, EKey::W, true);
	EXPECT_TRUE(Sample().Get("Interact").bActive);
	EXPECT_EQ(Sample().Get("Move").Value, FVector2d(0.0, 1.0));
}

TEST_F(FInputActionTests, ModalCancelsOnceAndRemovalRequiresFreshPress)
{
	FAccess::Key(Input, EKey::W, true);
	FAccess::Key(Input, EKey::Space, true);
	Sample();
	FInputMappingContext Menu;
	Menu.Name = "Menu";
	Menu.Priority = 200;
	Menu.bBlockKeyboard = Menu.bBlockMouse = true;
	ASSERT_TRUE(Actions.AddContext(Menu));
	EXPECT_TRUE(Actions.GetSnapshot().Get("Jump").bCancelled);
	EXPECT_EQ(Actions.GetSnapshot().Get("Move").Value, FVector2d(0.0));
	Next();
	EXPECT_TRUE(Sample().Get("Jump").bCancelled);
	Next();
	Actions.Cancel();
	EXPECT_FALSE(Sample().Get("Jump").bCancelled);
	ASSERT_TRUE(Actions.RemoveContext("Menu"));
	Next();
	EXPECT_FALSE(Sample().Get("Move").bActive);
	FAccess::Key(Input, EKey::W, false);
	Next();
	FAccess::Key(Input, EKey::W, true);
	EXPECT_TRUE(Sample().Get("Move").bActive);
}

TEST_F(FInputActionTests, UiCaptureKeepsPhysicalReleaseAndDoesNotResumeHeldKeys)
{
	FAccess::Key(Input, EKey::W, true);
	FAccess::Key(Input, EKey::Space, true);
	Sample();
	Next();
	FAccess::Block(Input, true, true);
	EXPECT_TRUE(Sample().Get("Move").bCancelled);
	EXPECT_TRUE(Input.IsKeyDown(EKey::Space));
	Next();
	FAccess::Key(Input, EKey::Space, false);
	EXPECT_TRUE(Input.WasKeyReleased(EKey::Space));
	EXPECT_FALSE(Sample().Get("Jump").bActive);
	Next();
	FAccess::Block(Input, false, false);
	EXPECT_FALSE(Sample().Get("Move").bActive);
	Next();
	FAccess::Key(Input, EKey::Space, true);
	EXPECT_TRUE(Sample().Get("Jump").bStarted);
}

TEST_F(FInputActionTests, UnrelatedLostSourceDoesNotTurnAReleaseIntoCancellation)
{
	FAccess::Key(Input, EKey::W, true);
	FAccess::Mouse(Input, EMouseButton::Left, true);
	Sample();
	Next();
	FAccess::Block(Input, true, false);
	FAccess::Mouse(Input, EMouseButton::Left, false);
	EXPECT_TRUE(Sample().Get("Move").bCancelled);
	EXPECT_TRUE(Sample().Get("Interact").bCompleted);
	EXPECT_FALSE(Sample().Get("Interact").bCancelled);
}

TEST_F(FInputActionTests, MouseCaptureDoesNotCancelKeyboardMovement)
{
	FAccess::Key(Input, EKey::W, true);
	Sample();
	Next();
	Actions.SetDeviceBlocked(false, true);
	EXPECT_TRUE(Actions.GetSnapshot().Get("Move").bActive);
	EXPECT_TRUE(Sample().Get("Move").bActive);
	EXPECT_FALSE(Sample().Get("Move").bCancelled);
}

TEST_F(FInputActionTests, FocusResetRejectsRepeatsButAcceptsFreshPress)
{
	FAccess::Key(Input, EKey::Space, true);
	FAccess::Move(Input, {5.0, 5.0});
	Sample();
	FAccess::Focus(Input, false);
	FAccess::Focus(Input, true);
	FAccess::Key(Input, EKey::Space, true, true);
	FAccess::Move(Input, {500.0, 500.0});
	EXPECT_TRUE(Sample().Get("Jump").bCancelled);
	EXPECT_FALSE(Sample().Get("Jump").bActive);
	EXPECT_EQ(Sample().Get("Look").Value, FVector2d(0.0));
	Next();
	FAccess::Key(Input, EKey::Space, true);
	EXPECT_TRUE(Sample().Get("Jump").bStarted);
}

TEST_F(FInputActionTests, RebindingCancelsAndSuppressesBothOldAndNewHeldSources)
{
	FAccess::Key(Input, EKey::W, true);
	FAccess::Key(Input, EKey::Up, true);
	Sample();
	std::string Error;
	ASSERT_TRUE(Actions.Rebind("Game", "Forward", FInputSource::Key(EKey::Up), Error)) << Error;
	EXPECT_EQ(Actions.GetSnapshot().Get("Move").Value, FVector2d(0.0));
	Next();
	EXPECT_FALSE(Sample().Get("Move").bActive);
	FAccess::Key(Input, EKey::Up, false);
	Next();
	FAccess::Key(Input, EKey::Up, true);
	EXPECT_TRUE(Sample().Get("Move").bActive);
	EXPECT_FALSE(Actions.Rebind("Game", "Forward", FInputSource::Key(EKey::S), Error));
	EXPECT_NE(Error.find("conflict"), std::string::npos);
	EXPECT_EQ(Actions.GetBindingSource("Game", "Forward"), FInputSource::Key(EKey::Up));
	ASSERT_TRUE(Actions.ResetBindings(Error));
	EXPECT_EQ(Actions.GetBindingSource("Game", "Forward"), FInputSource::Key(EKey::W));
}

TEST_F(FInputActionTests, OverrideFilesRoundTripAndRejectMalformedDataTransactionally)
{
	const auto Root = Durin::Testing::CreateTestFixtureDirectory("InputOverrides");
	const auto Path = Root / "User.bindings";
	std::string Error;
	ASSERT_TRUE(Actions.Rebind("Game", "Forward", FInputSource::Key(EKey::Up), Error));
	ASSERT_TRUE(Actions.SaveOverrides(Path, Error)) << Error;
	ASSERT_TRUE(Actions.ResetBindings(Error));
	ASSERT_TRUE(Actions.LoadOverrides(Path, Error)) << Error;
	EXPECT_EQ(Actions.GetBindingSource("Game", "Forward"), FInputSource::Key(EKey::Up));
	for (const std::string Text : {
		"DurinInputBindings 2\n",
		"DurinInputBindings 1\n\"Unknown\" \"Forward\" 0 15\n",
		"DurinInputBindings 1\n\"Game\" \"Jump\" 2 0\n",
		"DurinInputBindings 1\n\"Game\" \"Forward\" 0 83\n",
		"DurinInputBindings 1\n\"Game\" \"Forward\" 0 15\n\"Game\" \"Forward\" 0 16\n",
		"DurinInputBindings 1\n\"Game\" \"Forward\" 0 99999\n",
		"DurinInputBindings 1\ntruncated"})
	{
		ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(std::as_bytes(std::span(Text.data(), Text.size())), Path));
		EXPECT_FALSE(Actions.LoadOverrides(Path, Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_EQ(Actions.GetBindingSource("Game", "Forward"), FInputSource::Key(EKey::Up));
	}
	EXPECT_FALSE(Actions.SaveOverrides(Path / "Child", Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Actions.GetBindingSource("Game", "Forward"), FInputSource::Key(EKey::Up));
}

TEST_F(FInputActionTests, InvalidDefinitionsAndRebindingsDoNotChangeState)
{
	std::string Error;
	EXPECT_FALSE(Actions.Rebind("Game", "Missing", FInputSource::Key(EKey::W), Error));
	EXPECT_FALSE(Actions.Rebind("Game", "Jump", {EInputSourceKind::MouseX}, Error));
	EXPECT_FALSE(Actions.Rebind("Game", "Forward", FInputSource::Key(EKey::None), Error));
	FInputMappingContext Bad;
	Bad.Name = "Bad";
	Bad.Bindings = {{"Missing", "DoesNotExist", FInputSource::Key(EKey::W)}};
	EXPECT_FALSE(Actions.AddContext(Bad));
	Bad.Bindings = {{"Jump", "Jump", FInputSource::Key(EKey::K), {1.0, 1.0}}};
	EXPECT_FALSE(Actions.AddContext(Bad));
	EXPECT_FALSE(Actions.DefineAction("Jump", EInputActionType::Button));
	EXPECT_FALSE(Actions.SetContextActive("Missing", true));
	EXPECT_FALSE(Actions.GetSnapshot().Get("Missing").bActive);
}

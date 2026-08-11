#include "Input/GameInputState.h"

namespace Durin
{
	auto FGameInputState::SetEnabled(bool bInEnabled) -> void
	{
		if (bEnabled == bInEnabled) return;
		bEnabled = bInEnabled;
		if (!bEnabled) Reset();
	}

	auto FGameInputState::SetFocused(bool bInFocused) -> void
	{
		bFocused = bInFocused;
		if (!bFocused) Reset();
	}

	auto FGameInputState::SetKey(EKey Key, bool bDown) -> void
	{
		if (!bEnabled || !bFocused) return;
		const size_t Index = ToKeyIndex(Key);
		if (KeyDown[Index] == bDown) return;
		KeyDown[Index] = bDown;
		(bDown ? KeyPressed : KeyReleased)[Index] = true;
	}

	auto FGameInputState::SetMouseButton(EMouseButton Button, bool bDown) -> void
	{
		if (!bEnabled || !bFocused) return;
		const size_t Index = static_cast<size_t>(Button);
		if (MouseDown[Index] == bDown) return;
		MouseDown[Index] = bDown;
		(bDown ? MousePressed : MouseReleased)[Index] = true;
	}

	auto FGameInputState::SetMousePosition(FVector2d Position) -> void
	{
		if (!bEnabled || !bFocused) return;
		if (bHasMousePosition) MouseDelta += Position - MousePosition;
		MousePosition = Position;
		bHasMousePosition = true;
	}

	auto FGameInputState::AddMouseWheel(double Delta) -> void
	{
		if (bEnabled && bFocused) MouseWheelDelta += Delta;
	}

	auto FGameInputState::FinishGameTick() -> void
	{
		KeyPressed.fill(false);
		KeyReleased.fill(false);
		MousePressed.fill(false);
		MouseReleased.fill(false);
		MouseDelta = FVector2d(0.0);
		MouseWheelDelta = 0.0;
	}

	auto FGameInputState::ResetMouseTracking() -> void
	{
		MousePosition = FVector2d(0.0);
		MouseDelta = FVector2d(0.0);
		bHasMousePosition = false;
	}

	auto FGameInputState::Reset() -> void
	{
		KeyDown.fill(false);
		MouseDown.fill(false);
		FinishGameTick();
		ResetMouseTracking();
	}
}

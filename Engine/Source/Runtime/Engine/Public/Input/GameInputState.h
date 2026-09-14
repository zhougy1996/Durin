#pragma once

#include "EngineAPI.h"
#include "Input/InputCoreTypes.h"
#include "Math/Vector.h"

namespace Durin
{
	struct FGameInputStateTestAccess;

	// Ordered digital transitions preserve taps and overlapping action bindings.
	struct FGameInputTransition
	{
		uint16 Source = 0; // Keys 0..255; mouse buttons 256..258.
		bool bDown = false;
	};

	// Accumulates enabled-window input and exposes current plus one-tick transition state.
	class FGameInputState
	{
	public:
		auto IsKeyDown(EKey Key) const -> bool { return KeyDown[ToKeyIndex(Key)]; }
		auto WasKeyPressed(EKey Key) const -> bool { return KeyPressed[ToKeyIndex(Key)]; }
		auto WasKeyReleased(EKey Key) const -> bool { return KeyReleased[ToKeyIndex(Key)]; }
		auto IsMouseButtonDown(EMouseButton Button) const -> bool { return MouseDown[static_cast<size_t>(Button)]; }
		auto WasMouseButtonPressed(EMouseButton Button) const -> bool { return MousePressed[static_cast<size_t>(Button)]; }
		auto WasMouseButtonReleased(EMouseButton Button) const -> bool { return MouseReleased[static_cast<size_t>(Button)]; }
		auto GetMousePosition() const -> const FVector2d& { return MousePosition; }
		auto GetMouseDelta() const -> const FVector2d& { return MouseDelta; }
		auto GetMouseWheelDelta() const -> double { return MouseWheelDelta; }
		auto IsEnabled() const -> bool { return bEnabled; }
		auto IsFocused() const -> bool { return bFocused; }
		auto GetTransitions() const -> const std::vector<FGameInputTransition>& { return Transitions; }
		auto GetFrameNumber() const -> uint64 { return FrameNumber; }
		auto GetResetNumber() const -> uint64 { return ResetNumber; }
		auto IsKeyboardBlocked() const -> bool { return bKeyboardBlocked; }
		auto IsMouseBlocked() const -> bool { return bMouseBlocked; }

	private:
		static auto ToKeyIndex(EKey Key) -> size_t { return std::min(static_cast<size_t>(Key), KeyCapacity - 1); }
		ENGINE_API auto SetEnabled(bool bInEnabled) -> void;
		ENGINE_API auto SetFocused(bool bInFocused) -> void;
		ENGINE_API auto SetKey(EKey Key, bool bDown, bool bRepeat = false) -> void;
		ENGINE_API auto SetMouseButton(EMouseButton Button, bool bDown) -> void;
		ENGINE_API auto SetMousePosition(FVector2d Position) -> void;
		ENGINE_API auto AddMouseWheel(double Delta) -> void;
		ENGINE_API auto FinishGameTick() -> void;
		ENGINE_API auto ResetMouseTracking() -> void;
		auto Reset() -> void;

		inline static constexpr size_t KeyCapacity = 256;
		std::array<bool, KeyCapacity> KeyDown{};
		std::array<bool, KeyCapacity> KeyPressed{};
		std::array<bool, KeyCapacity> KeyReleased{};
		std::array<bool, 3> MouseDown{};
		std::array<bool, 3> MousePressed{};
		std::array<bool, 3> MouseReleased{};
		FVector2d MousePosition{0.0};
		FVector2d MouseDelta{0.0};
		double MouseWheelDelta = 0.0;
		bool bHasMousePosition = false;
		bool bEnabled = false;
		bool bFocused = false;
		bool bKeyboardBlocked = false;
		bool bMouseBlocked = false;
		uint64 FrameNumber = 1;
		uint64 ResetNumber = 0;
		std::vector<FGameInputTransition> Transitions;

		friend class DEngine;
		friend class FEngineInputEventHandler;
		friend struct FGameInputStateTestAccess;
	};
}

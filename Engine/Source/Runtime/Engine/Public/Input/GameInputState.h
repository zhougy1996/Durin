#pragma once

#include "EngineAPI.h"
#include "Input/InputCoreTypes.h"
#include "Math/Vector.h"

namespace Durin
{
	struct FGameInputStateTestAccess;

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

	private:
		static auto ToKeyIndex(EKey Key) -> size_t { return std::min(static_cast<size_t>(Key), KeyCapacity - 1); }
		ENGINE_API auto SetEnabled(bool bInEnabled) -> void;
		ENGINE_API auto SetFocused(bool bInFocused) -> void;
		ENGINE_API auto SetKey(EKey Key, bool bDown) -> void;
		auto SetMouseButton(EMouseButton Button, bool bDown) -> void;
		ENGINE_API auto SetMousePosition(FVector2d Position) -> void;
		auto AddMouseWheel(double Delta) -> void;
		ENGINE_API auto FinishGameTick() -> void;
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

		friend class DEngine;
		friend class FEngineInputEventHandler;
		friend struct FGameInputStateTestAccess;
	};
}

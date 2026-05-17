#pragma once

#include "ApplicationCoreAPI.h"
#include "Input/InputCoreTypes.h"

namespace Durin
{
	class FGenericWindow;

	class FGenericApplicationMessageHandler
	{
	public:
		virtual ~FGenericApplicationMessageHandler() = default;

		virtual auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void {};

		virtual auto OnWindowFocus(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused) -> void {}
		virtual auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool { return false; }
		virtual auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool { return false; }
		virtual auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool { return false; }

		virtual auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool { return false; }
		virtual auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos ) -> bool { return false; }
		virtual auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool { return false; }
		virtual auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY) -> bool { return false; }
	};
} // namespace Doge
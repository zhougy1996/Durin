#pragma once

#include "ApplicationCoreAPI.h"
#include "Input/InputCoreTypes.h"

namespace Doge
{
	class FGenericWindow;

	class FGenericApplicationMessageHandler
	{
	public:
		virtual ~FGenericApplicationMessageHandler() = default;

		virtual auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void {};
		virtual auto OnCharEvent(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> void {}
		virtual auto OnMouseButton(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 Button, int32 Action, int32 Mods) -> void {}
		virtual auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, float X, float Y) -> void {}
		virtual auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, float XOffset, float YOffset) -> void {}

		virtual auto OnWindowFocus(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused) -> void {}
		virtual auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool { return false; }
		virtual auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool { return false; }
		virtual auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool { return false; }
	};
} // namespace Doge
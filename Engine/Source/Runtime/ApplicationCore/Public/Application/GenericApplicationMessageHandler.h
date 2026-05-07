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

		virtual auto OnWindowResize(const FGenericWindow* InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void {};
		virtual auto OnKeyEvent(const FGenericWindow* InPlatformWindow, EKey Key, EKeyAction Action, EKeyModFlags Mods) -> void {}
		virtual auto OnCharEvent(const FGenericWindow* InPlatformWindow, uint32 Codepoint) -> void {}
		virtual auto OnMouseButton(const FGenericWindow* InPlatformWindow, int32 Button, int32 Action, int32 Mods) -> void {}
		virtual auto OnMouseMove(const FGenericWindow* InPlatformWindow, float X, float Y) -> void {}
		virtual auto OnMouseWheel(const FGenericWindow* InPlatformWindow, float XOffset, float YOffset) -> void {}
		virtual auto OnWindowFocus(const FGenericWindow* InPlatformWindow, bool bFocused) -> void {}
	};
}
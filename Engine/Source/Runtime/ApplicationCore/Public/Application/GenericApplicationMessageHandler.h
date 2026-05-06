#pragma once

#include "ApplicationCoreAPI.h"

namespace Doge
{
	class FGenericWindow;

	class FGenericApplicationMessageHandler
	{
	public:
		virtual ~FGenericApplicationMessageHandler() = default;

		virtual auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void {};
		virtual auto OnKeyEvent(int32 Key, int32 Scancode, int32 Action, int32 Mods) -> void {}
		virtual auto OnCharEvent(uint32 Codepoint) -> void {}
		virtual auto OnMouseButton(int32 Button, int32 Action, int32 Mods) -> void {}
		virtual auto OnMouseMove(float X, float Y) -> void {}
		virtual auto OnMouseWheel(float XOffset, float YOffset) -> void {}
		virtual auto OnWindowFocus(bool bFocused) -> void {}
	};
}
#pragma once

#include "Input/InputCoreTypes.h"

namespace Durin::Mona
{
	class FMonaEventHandler
	{
	public:
		FMonaEventHandler() = default;
		virtual ~FMonaEventHandler() = default;

		DURIN_NONCOPYABLE(FMonaEventHandler)

		virtual auto OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> bool { return false; }

		virtual auto OnWindowRefresh(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void {}

		virtual auto OnWindowFocused(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused) -> void {}

		virtual auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool { return false; }

		virtual auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool { return false; }

		virtual auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool { return false; }

		virtual auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool { return false; }

		virtual auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool { return false; }

		virtual auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool { return false; }

		virtual auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY) -> bool { return false; }
	};
} // namespace Durin::Mona

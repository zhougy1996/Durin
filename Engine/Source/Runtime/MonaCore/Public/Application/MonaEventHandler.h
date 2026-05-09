#pragma once

#include "Input/InputCoreTypes.h"

namespace Doge::Mona
{
	class FMonaEventHandler
	{
	public:
		FMonaEventHandler() = default;
		virtual ~FMonaEventHandler() = default;

		DOGE_NONCOPYABLE(FMonaEventHandler)

		virtual auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool { return false; }

		virtual auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool { return false; }

		virtual auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool { return false; }

		virtual bool OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) { return false; }

		virtual bool OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos ) { return false; }

		virtual bool OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) { return false; }
	};
} // namespace Doge::Mona
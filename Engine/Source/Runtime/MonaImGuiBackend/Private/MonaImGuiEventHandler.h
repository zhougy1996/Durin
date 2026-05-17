#pragma once

#include "ApplicationCoreFwd.h"
#include "Application/MonaEventHandler.h"

namespace Durin::Mona
{
	class FMonaImGuiEventHandler : public FMonaEventHandler
	{
	public:
		FMonaImGuiEventHandler()
			: FMonaEventHandler()
		{
		}

		~FMonaImGuiEventHandler() override {}

		auto OnWindowFocused(const std::shared_ptr<FGenericWindow> &InPlatformWindow, bool bFocused) -> void override;

		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool override;

		auto OnKeyUp(const std::shared_ptr<FGenericWindow> &InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool override;

		auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool override;

		auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool override;

		auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos ) -> bool override;

		auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;

		auto OnMouseWheel(const std::shared_ptr<FGenericWindow> &InPlatformWindow, double DeltaX, double DeltaY) -> bool override;
	};

	using FMonaBackendEventHandler = FMonaImGuiEventHandler;
}

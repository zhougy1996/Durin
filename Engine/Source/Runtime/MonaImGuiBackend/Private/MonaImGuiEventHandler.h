#pragma once

#include "ApplicationCoreFwd.h"
#include "Application/MonaEventHandler.h"

namespace Doge::Mona
{
	class FMonaImGuiEventHandler : public FMonaEventHandler
	{
	public:
		FMonaImGuiEventHandler()
			: FMonaEventHandler()
		{
		}

		~FMonaImGuiEventHandler() override {}

		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool override;

		auto OnKeyUp(const std::shared_ptr<FGenericWindow> &InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool override;

		auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool override;
	};
} // namespace Doge::Mona

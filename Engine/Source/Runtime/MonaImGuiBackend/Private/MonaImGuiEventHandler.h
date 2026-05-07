#pragma once

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

		auto OnKeyEvent(const FGenericWindow* InPlatformWindow, EKey Key, EKeyAction Action, EKeyModFlags Mods) -> void override;

	private:
	};
} // namespace Doge::Mona

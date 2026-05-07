#pragma once

#include "Input/InputCoreTypes.h"
#include "ApplicationCoreFwd.h"

namespace Doge::Mona
{
	class FMonaEventHandler
	{
	public:
		FMonaEventHandler() = default;
		virtual ~FMonaEventHandler() = default;

		DOGE_NONCOPYABLE(FMonaEventHandler)

		virtual auto OnKeyEvent(const FGenericWindow* InPlatformWindow, EKey Key, EKeyAction Action, EKeyModFlags Mods) -> void {}
	};
}
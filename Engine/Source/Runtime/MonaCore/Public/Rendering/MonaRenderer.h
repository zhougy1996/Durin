#pragma once

#include "MonaCoreAPI.h"

namespace Doge::Mona
{
	class MWindow;

	class FMonaRenderer
	{
	public:
		virtual ~FMonaRenderer() = default;

		MONACORE_API virtual auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void = 0;

		MONACORE_API virtual auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void = 0;

		MONACORE_API virtual auto DrawWindows() -> void = 0;

		MONACORE_API virtual auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void = 0;
	};
}
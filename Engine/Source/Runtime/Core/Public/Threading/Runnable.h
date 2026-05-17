#pragma once

#include "HAL/Platform.h"

namespace Durin
{
	class FRunnable
	{
	public:
		virtual ~FRunnable() = default;

		virtual auto Init() -> bool
		{
			return true;
		}

		virtual auto Run() -> uint32 = 0;

		virtual auto Stop() -> void {}

		virtual auto Exit() -> void {}
	};

}
#pragma once

#include "MonaCoreAPI.h"

namespace Durin::Mona
{
	class IMonaUIBackend
	{
	public:
		virtual ~IMonaUIBackend() = default;

		virtual auto Initialize() -> void = 0;
		virtual auto Shutdown() -> void = 0;
		virtual auto NewFrame() -> void = 0;
		virtual auto Render() -> void = 0;
	};
}

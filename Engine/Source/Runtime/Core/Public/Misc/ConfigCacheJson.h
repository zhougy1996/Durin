#pragma once

namespace Doge
{
	class CORE_API FConfigCacheJson
	{
	public:
		static auto LoadAndParseConfig() -> void;
	};
}
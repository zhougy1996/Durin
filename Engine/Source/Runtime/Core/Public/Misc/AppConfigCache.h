#pragma once

namespace Doge
{
	class FAppConfigCache
	{
	public:
		std::string AppName = "Doge";

		std::string LogLevel = "Debug";
	};

	CORE_API extern FAppConfigCache GAppConfig;

	CORE_API auto IsAppConfigLoaded() -> bool;

	namespace CoreInternal
	{
		CORE_API auto LoadApplicationConfig(const std::string& ConfigFile) -> bool;
	}
}
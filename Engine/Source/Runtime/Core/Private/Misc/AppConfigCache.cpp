#include "Misc/AppConfigCache.h"

#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>

#include "Misc/FileHelper.h"

template<>
struct glz::meta<Doge::FAppConfigCache>
{
	using T = Doge::FAppConfigCache;
	static constexpr auto value = object(&T::AppName, &T::LogLevel);
};

namespace Doge
{
	FAppConfigCache GAppConfig;

	static bool bIsAppConfigLoaded = false;

	auto IsAppConfigLoaded() -> bool
	{
		return bIsAppConfigLoaded;
	}

	auto CoreInternal::LoadApplicationConfig(const std::string& ConfigFile) -> bool
	{
		std::string ConfigContent;
		bool bLoadSuccess = FFileHelper::LoadFileToString(ConfigContent, ConfigFile);
		if (!bLoadSuccess)
		{
			DOGE_ERROR("Failed to load application config file: {}", ConfigFile);
			return false;
		}

		if (ConfigContent.empty())
		{
			DOGE_ERROR("Application config file is empty: {}", ConfigFile);
			return false;
		}

		glz::error_ctx ErrorCode = glz::read_yaml(GAppConfig, ConfigContent);
		if (ErrorCode)
		{
			std::string ErrorMsg = glz::format_error(ErrorCode, ConfigFile);
			DOGE_ERROR("Failed to parse application config file: {}\nError: {}", ConfigFile, ErrorMsg);
			return false;
		}
		bIsAppConfigLoaded = true;
		return true;
	}
} // namespace Doge

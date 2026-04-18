#include "Misc/AppConfigCache.h"

#include "Misc/FileHelper.h"

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

		glz::error_ctx ErrorCode = glz::read_yaml(GAppConfig, ConfigContent);
		if (ErrorCode)
		{
			std::string ErrorMsg = glz::format_error(ErrorCode, ConfigFile);
			DOGE_ERROR("Failed to load application config: {}", ErrorMsg);
			return false;
		}
		bIsAppConfigLoaded = true;
		return true;
	}
} // namespace Doge


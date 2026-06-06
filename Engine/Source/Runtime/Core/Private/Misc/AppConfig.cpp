#include "Misc/AppConfig.h"

#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		auto GetAppConfigDocument() -> FYamlDocument&
		{
			static FYamlDocument AppConfigDocument;
			return AppConfigDocument;
		}
	}

	FYamlNodeView GAppConfig;

	auto LoadAppConfig(std::string_view ConfigFile) -> bool
	{
		FYamlDocument& AppConfigDocument = GetAppConfigDocument();
		check(!AppConfigDocument.IsValid());

		FYamlParseError ParseError;
		if (!AppConfigDocument.LoadFromFile(ConfigFile, &ParseError))
		{
			GAppConfig = {};
			DURIN_ERROR("Failed to load application config file {}: {}", ConfigFile, ParseError.Message);
			return false;
		}

		GAppConfig = AppConfigDocument.GetRootView();
		return true;
	}
}

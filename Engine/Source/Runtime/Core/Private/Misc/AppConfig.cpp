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

		FYamlNodeView GAppConfigRoot;
	}

	auto LoadAppConfig(std::string_view ConfigFile) -> bool
	{
		FYamlDocument& AppConfigDocument = GetAppConfigDocument();
		check(!AppConfigDocument.IsValid());

		FYamlParseError ParseError;
		if (!AppConfigDocument.LoadFromFile(ConfigFile, &ParseError))
		{
			GAppConfigRoot = {};
			DURIN_ERROR("Failed to load application config file {}: {}", ConfigFile, ParseError.Message);
			return false;
		}

		GAppConfigRoot = AppConfigDocument.GetRootView();
		return true;
	}

	auto GetModuleConfig(std::string_view ModuleName) -> FYamlNodeView
	{
		return GAppConfigRoot.GetView(ModuleName);
	}
}

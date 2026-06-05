#include "Misc/AppConfigCache.h"

namespace Durin
{
	FYamlNodeView GAppConfig;

	namespace
	{
		auto GetAppConfigDocument() -> FYamlDocument&
		{
			static FYamlDocument AppConfigDocument;
			return AppConfigDocument;
		}
	}

	namespace CoreInternal
	{
		auto LoadApplicationConfig(std::string_view ConfigFile) -> bool
		{
			FYamlDocument& GAppConfigDocument = GetAppConfigDocument();
			check(!GAppConfigDocument.IsValid());

			FYamlParseError ParseError;
			if (!GAppConfigDocument.LoadFromFile(ConfigFile, &ParseError))
			{
				GAppConfig = {};
				DURIN_ERROR("Failed to load application config file {}: {}", ConfigFile, ParseError.Message);
				return false;
			}

			GAppConfig = GAppConfigDocument.GetRootView();
			return true;
		}
	} // namespace CoreInternal

	auto IsAppConfigLoaded() -> bool
	{
		const FYamlDocument& GAppConfigDocument = GetAppConfigDocument();
		return GAppConfigDocument.IsValid();
	}
} // namespace Durin

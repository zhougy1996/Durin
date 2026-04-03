#include "Misc/ConfigCacheJson.h"

#include "Json/Json.h"

namespace Doge
{
	auto FConfigCacheJson::LoadFile(std::string_view InFilePath) -> void
	{
		auto ConfigDoc = FJson::ParseFile(InFilePath);
		if (ConfigDoc.IsNull())
		{
			DOGE_WARN("Failed to load config file: {}", InFilePath);
			return;
		}
		for (auto It = ConfigDoc.MemberBegin(); It != ConfigDoc.MemberEnd(); ++It)
		{
			FName Key(It->name.GetString());
			std::string Value;
			if (It->value.IsString())
			{
				Value = It->value.GetString();
			}
			// TODO: support other types of config values
			CachedConfigs[Key] = Value;
		}

		bIsLoaded = true;
	}

	auto FConfigCacheJson::GetStringValue(FName InKey) -> std::string
	{
		const auto It = CachedConfigs.find(InKey);
		if (It != CachedConfigs.end())
		{
			return It->second;
		}
		return {};
	}

	static constexpr std::string_view DogeConfigFile = "DogeConfig.json";

	FConfigCacheJson* GConfigs = nullptr;

	auto GlobalConfigsInit() -> void
	{
		GConfigs = new FConfigCacheJson();
		GConfigs->LoadFile(DogeConfigFile);
		check(GConfigs->IsLoaded() && "Failed to load global configs");
	}

	auto GlobalConfigsDeinit() -> void
	{
		delete GConfigs;
		GConfigs = nullptr;
	}

} // namespace Doge
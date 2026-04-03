#pragma once

namespace Doge
{
	class FConfigCacheJson
	{
	public:
		FConfigCacheJson() = default;

		CORE_API auto LoadFile(std::string_view InFilePath) -> void;

		CORE_API auto GetStringValue(FName InKey) -> std::string;

		auto IsLoaded() const -> bool { return bIsLoaded; }
	private:
		std::unordered_map<FName, std::string> CachedConfigs;

		bool bIsLoaded{ false };
	};

	CORE_API auto GlobalConfigsInit() -> void;

	CORE_API auto GlobalConfigsDeinit() -> void;
}
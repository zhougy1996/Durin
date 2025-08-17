#include "Misc/ConfigCacheJson.h"

#include "CoreGlobals.h"

static auto GetShaderPathFromJson(const FString& JsonFilePath) -> FPath
{
	rapidjson::Document Doc = FJson::ParseJson(JsonFilePath);

	if (!Doc.HasMember("ShaderPath") || !Doc["ShaderPath"].IsString())
	{
		throw std::runtime_error("Missing or invalid 'ShaderPath' field in JSON");
	}

	return FPath(Doc["ShaderPath"].GetString());
}

auto FConfigCacheJson::LoadAndParseConfig() -> void
{
	GShaderPath = GetShaderPathFromJson(STR("DogeConfig.json"));
}

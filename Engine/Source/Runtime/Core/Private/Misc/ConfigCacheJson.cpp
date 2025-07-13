#include "Misc/ConfigCacheJson.h"

#include "CoreGlobals.h"

static auto GetShaderPathFromJson(const FString& JsonFilePath) -> FPath
{
	FILE* fp = fopen(JsonFilePath.c_str(), "rb");
	if (!fp)
	{
		throw std::runtime_error("Failed to open JSON file: " + JsonFilePath);
	}

	char readBuffer[65536];
	rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));

	rapidjson::Document doc;
	doc.ParseStream(is);
	fclose(fp);

	if (doc.HasParseError())
	{
		throw std::runtime_error("JSON parse error: " +
									std::to_string(doc.GetParseError()));
	}

	if (!doc.HasMember("ShaderPath") || !doc["ShaderPath"].IsString())
	{
		throw std::runtime_error("Missing or invalid 'ShaderPath' field in JSON");
	}

	return FPath(doc["ShaderPath"].GetString());
}

auto FConfigCacheJson::LoadAndParseConfig() -> void
{
	GShaderPath = GetShaderPathFromJson("DogeConfig.json");
	FPath test = GShaderPath / "shaders";
}

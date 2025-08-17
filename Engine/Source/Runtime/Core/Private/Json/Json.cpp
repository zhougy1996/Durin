#include "Json/Json.h"

namespace FJson
{
auto ParseJson(const FString& JsonFilePath) -> rapidjson::Document
{
	FILE* FilePointer = fopen(ToCStr(JsonFilePath), "rb");
	if (!FilePointer)
	{
		throw std::runtime_error(std::format("Failed to open JSON file: {}", ToCStr(JsonFilePath)));
	}
	std::unique_ptr<char[]> readBuffer(new char[65536]);
	rapidjson::FileReadStream Stream(FilePointer, readBuffer.get(), 65536);
	rapidjson::Document Doc;
	Doc.ParseStream(Stream);
	fclose(FilePointer);
	if (Doc.HasParseError())
	{
		throw std::runtime_error(std::format("JSON parse error: {}", std::to_string(Doc.GetParseError())));
	}
	return Doc;
}

} // namespace FJson
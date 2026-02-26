#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"

namespace Doge
{
	namespace FJson
	{
		auto ParseJson(const FString& JsonFilePath) -> rapidjson::Document;

	}
}
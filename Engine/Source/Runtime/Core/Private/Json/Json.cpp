#include "Json/Json.h"

#include "Misc//FileHelper.h"

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/error/en.h"

namespace Doge
{
	namespace FJson
	{
		auto ParseFile(std::string_view FilePath) -> FJsonDocument
		{
			rapidjson::Document Doc;
			std::string FileContents;
			const bool bLoadSuccess = FFileHelper::LoadFileToString(FileContents, FilePath);
			if (!bLoadSuccess)
			{
				DOGE_WARN("Failed to load JSON file: {}", FilePath);
				return Doc;
			}
			return ParseString(FileContents);
		}

		auto ParseString(std::string_view JsonString) -> FJsonDocument
		{
			FJsonDocument Doc;
			Doc.Parse(JsonString.data());
			if (Doc.HasParseError())
			{
				rapidjson::ParseErrorCode Code = Doc.GetParseError();

				const char* ErrorMsg = rapidjson::GetParseError_En(Code);
				size_t ErrorOffset = Doc.GetErrorOffset();
				DOGE_WARN("Json parse failed at offset {}: {}", ErrorOffset, ErrorMsg);
				if (ErrorOffset < JsonString.size())
				{
					const size_t ErrorLen = std::min(static_cast<size_t>(50), JsonString.size() - ErrorOffset);
					DOGE_WARN("Invalid JSON string: {}", std::string(JsonString.data() + ErrorOffset, ErrorLen));
				}
			}
			return Doc;
		}
	}

	auto ToString(const FJsonDocument& InDocument) -> std::string
	{
		rapidjson::StringBuffer Buffer;
		rapidjson::Writer Writer(Buffer);
		InDocument.Accept(Writer);

		return Buffer.GetString();
	}
}
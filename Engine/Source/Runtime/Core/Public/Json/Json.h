#pragma once

#include "rapidjson/document.h"

namespace Doge
{
	using FJsonDocument = rapidjson::Document;

	namespace FJson
	{
		CORE_API auto ParseFile(std::string_view FilePath) -> FJsonDocument;

		CORE_API auto ParseString(std::string_view JsonString) -> FJsonDocument;
	}

	CORE_API auto ToString(const FJsonDocument& InDocument) -> std::string;
}
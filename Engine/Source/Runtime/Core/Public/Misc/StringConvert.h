#pragma once

#include "CoreAPI.h"

namespace Doge
{
	namespace StringConvert
	{
		CORE_API auto WideToUtf8(std::wstring_view WideStr) -> std::string;

		CORE_API auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring;

		CORE_API auto CodepointToUtf8(uint32 Codepoint) -> std::string;
	}
}
#pragma once

namespace Doge
{
	namespace StringConvert
	{
		CORE_API auto WideToUtf8(std::wstring_view WideStr) -> std::string;

		CORE_API auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring;
	}
}
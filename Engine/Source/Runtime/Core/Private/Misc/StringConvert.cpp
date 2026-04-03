#include "Misc/StringConvert.h"

namespace Doge
{
	namespace StringConvert
	{
		auto WideToUtf8(std::wstring_view WideStr) -> std::string
		{
			if (WideStr.empty()) return {};
			const int SizeNeeded = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(WideStr.data()), static_cast<int>(WideStr.size()), nullptr, 0, nullptr, nullptr);
			std::string Result(SizeNeeded, '\0');
			WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(WideStr.data()), static_cast<int>(WideStr.size()), Result.data(), SizeNeeded, nullptr, nullptr);
			return Result;
		}

		auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring
		{
			if (Utf8Str.empty()) return {};
			const int SizeNeeded = MultiByteToWideChar(CP_UTF8, 0, Utf8Str.data(), static_cast<int>(Utf8Str.size()), nullptr, 0);
			std::wstring Result(SizeNeeded, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, Utf8Str.data(), static_cast<int>(Utf8Str.size()), Result.data(), SizeNeeded);
			return Result;
		}
	}
}
#include "Misc/StringConvert.h"

namespace Durin
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

		auto CodepointToUtf8(uint32 Codepoint) -> std::string
		{
			std::string Result;
			if (Codepoint <= 0x7F) {
				Result += static_cast<char>(Codepoint);
			} else if (Codepoint <= 0x7FF) {
				Result += static_cast<char>(0xC0 | (Codepoint >> 6));
				Result += static_cast<char>(0x80 | (Codepoint & 0x3F));
			} else if (Codepoint <= 0xFFFF) {
				Result += static_cast<char>(0xE0 | (Codepoint >> 12));
				Result += static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F));
				Result += static_cast<char>(0x80 | (Codepoint & 0x3F));
			} else if (Codepoint <= 0x10FFFF) {
				Result += static_cast<char>(0xF0 | (Codepoint >> 18));
				Result += static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3F));
				Result += static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3F));
				Result += static_cast<char>(0x80 | (Codepoint & 0x3F));
			}
			return Result;
		}
	} // namespace StringConvert
}
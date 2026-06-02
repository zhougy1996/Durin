#include "Misc/StringConvert.h"

#include "Misc/AssertionMacros.h"

namespace Durin
{
	namespace String
	{
		namespace
		{
			constexpr char GHexDigits[] = "0123456789abcdef";

			auto IsHexChar(const char Value) -> bool
			{
				return (Value >= '0' && Value <= '9')
					|| (Value >= 'a' && Value <= 'f')
					|| (Value >= 'A' && Value <= 'F');
			}

			auto HexCharToNibbleUnchecked(const char Value) -> uint8
			{
				if (Value >= '0' && Value <= '9')
				{
					return static_cast<uint8>(Value - '0');
				}

				if (Value >= 'a' && Value <= 'f')
				{
					return static_cast<uint8>(10 + Value - 'a');
				}

				if (Value >= 'A' && Value <= 'F')
				{
					return static_cast<uint8>(10 + Value - 'A');
				}

				return 0;
			}
		}

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

		auto IsHex(std::string_view Value, std::optional<size_t> ExpectedLength) -> bool
		{
			if (ExpectedLength.has_value() && Value.size() != *ExpectedLength)
			{
				return false;
			}

			for (const char Character : Value)
			{
				if (!IsHexChar(Character))
				{
					return false;
				}
			}

			return true;
		}

		auto BytesToHex(std::span<const uint8> Bytes) -> std::string
		{
			std::string Result;
			Result.reserve(Bytes.size() * 2);
			for (const uint8 Byte : Bytes)
			{
				Result.push_back(GHexDigits[(Byte >> 4) & 0xf]);
				Result.push_back(GHexDigits[Byte & 0xf]);
			}
			return Result;
		}

		auto SanitizeFileName(std::string_view Value, std::string_view Fallback) -> std::string
		{
			std::string Result;
			Result.reserve(Value.size());
			for (const char Character : Value)
			{
				const bool bAlphaNumeric =
					(Character >= 'a' && Character <= 'z') ||
					(Character >= 'A' && Character <= 'Z') ||
					(Character >= '0' && Character <= '9');
				if (bAlphaNumeric || Character == '_' || Character == '-' || Character == '.')
				{
					Result.push_back(Character);
				}
				else
				{
					Result.push_back('_');
				}
			}

			return Result.empty() ? std::string(Fallback) : Result;
		}

		auto HexToBytes(std::string_view Hex, std::span<uint8> OutBytes) -> void
		{
			check(Hex.size() == OutBytes.size() * 2);

			for (size_t Index = 0; Index < OutBytes.size(); ++Index)
			{
				const uint8 HighNibble = HexCharToNibbleUnchecked(Hex[Index * 2]);
				const uint8 LowNibble = HexCharToNibbleUnchecked(Hex[Index * 2 + 1]);
				OutBytes[Index] = static_cast<uint8>((HighNibble << 4) | LowNibble);
			}
		}
	} // namespace StringConvert
}

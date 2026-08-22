#include "Misc/StringConvert.h"

#include "Misc/AssertionMacros.h"

namespace Durin::StringUtils
{
	namespace
	{
		constexpr char GHexDigits[] = "0123456789abcdef";
		constexpr uint32 GReplacementCodepoint = 0xfffd;

		auto AppendUtf8(std::string& Output, uint32 Codepoint) -> void
		{
			if (Codepoint <= 0x7f)
			{
				Output.push_back(static_cast<char>(Codepoint));
			}
			else if (Codepoint <= 0x7ff)
			{
				Output.push_back(static_cast<char>(0xc0 | (Codepoint >> 6)));
				Output.push_back(static_cast<char>(0x80 | (Codepoint & 0x3f)));
			}
			else if (Codepoint <= 0xffff)
			{
				Output.push_back(static_cast<char>(0xe0 | (Codepoint >> 12)));
				Output.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3f)));
				Output.push_back(static_cast<char>(0x80 | (Codepoint & 0x3f)));
			}
			else
			{
				Output.push_back(static_cast<char>(0xf0 | (Codepoint >> 18)));
				Output.push_back(static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3f)));
				Output.push_back(static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3f)));
				Output.push_back(static_cast<char>(0x80 | (Codepoint & 0x3f)));
			}
		}

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
#if defined(_WIN32)
		const int SizeNeeded = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(WideStr.data()), static_cast<int>(WideStr.size()), nullptr, 0, nullptr, nullptr);
		std::string Result(SizeNeeded, '\0');
		WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(WideStr.data()), static_cast<int>(WideStr.size()), Result.data(), SizeNeeded, nullptr, nullptr);
		return Result;
#else
		static_assert(sizeof(wchar_t) == sizeof(uint32));
		std::string Result;
		Result.reserve(WideStr.size());
		for (const wchar_t Character : WideStr)
		{
			const uint32 Codepoint = static_cast<uint32>(Character);
			AppendUtf8(Result,
				Codepoint <= 0x10ffff && !(Codepoint >= 0xd800 && Codepoint <= 0xdfff)
					? Codepoint : GReplacementCodepoint);
		}
		return Result;
#endif
	}

	auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring
	{
		if (Utf8Str.empty()) return {};
#if defined(_WIN32)
		const int SizeNeeded = MultiByteToWideChar(CP_UTF8, 0, Utf8Str.data(), static_cast<int>(Utf8Str.size()), nullptr, 0);
		std::wstring Result(SizeNeeded, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Utf8Str.data(), static_cast<int>(Utf8Str.size()), Result.data(), SizeNeeded);
		return Result;
#else
		static_assert(sizeof(wchar_t) == sizeof(uint32));
		std::wstring Result;
		Result.reserve(Utf8Str.size());
		for (size_t Index = 0; Index < Utf8Str.size();)
		{
			const uint8 Lead = static_cast<uint8>(Utf8Str[Index]);
			uint32 Codepoint = 0;
			size_t Length = 0;
			if (Lead <= 0x7f)
			{
				Codepoint = Lead;
				Length = 1;
			}
			else if (Lead >= 0xc2 && Lead <= 0xdf)
			{
				Codepoint = Lead & 0x1f;
				Length = 2;
			}
			else if (Lead >= 0xe0 && Lead <= 0xef)
			{
				Codepoint = Lead & 0x0f;
				Length = 3;
			}
			else if (Lead >= 0xf0 && Lead <= 0xf4)
			{
				Codepoint = Lead & 0x07;
				Length = 4;
			}

			bool bValid = Length != 0 && Index + Length <= Utf8Str.size();
			for (size_t Offset = 1; bValid && Offset < Length; ++Offset)
			{
				const uint8 Continuation = static_cast<uint8>(Utf8Str[Index + Offset]);
				bValid = (Continuation & 0xc0) == 0x80;
				Codepoint = (Codepoint << 6) | (Continuation & 0x3f);
			}
			const uint32 Minimum = Length == 2 ? 0x80
				: Length == 3 ? 0x800
				: Length == 4 ? 0x10000 : 0;
			bValid = bValid && Codepoint >= Minimum && Codepoint <= 0x10ffff
				&& !(Codepoint >= 0xd800 && Codepoint <= 0xdfff);
			Result.push_back(static_cast<wchar_t>(
				bValid ? Codepoint : GReplacementCodepoint));
			Index += bValid ? Length : 1;
		}
		return Result;
#endif
	}

	auto CodepointToUtf8(uint32 Codepoint) -> std::string
	{
		std::string Result;
		if (Codepoint <= 0x10ffff
			&& !(Codepoint >= 0xd800 && Codepoint <= 0xdfff))
			AppendUtf8(Result, Codepoint);
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

	auto BytesToHex(std::span<const std::byte> Bytes) -> std::string
	{
		std::string Result;
		Result.reserve(Bytes.size() * 2);
		for (const std::byte ByteValue : Bytes)
		{
			const uint8 Byte = std::to_integer<uint8>(ByteValue);
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

	auto HexToBytes(std::string_view Hex, std::span<std::byte> OutBytes) -> void
	{
		check(Hex.size() == OutBytes.size() * 2);

		for (size_t Index = 0; Index < OutBytes.size(); ++Index)
		{
			const uint8 HighNibble = HexCharToNibbleUnchecked(Hex[Index * 2]);
			const uint8 LowNibble = HexCharToNibbleUnchecked(Hex[Index * 2 + 1]);
			OutBytes[Index] = static_cast<std::byte>((HighNibble << 4) | LowNibble);
		}
	}
} // namespace Durin::StringUtils

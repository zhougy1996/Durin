#pragma once

#include <cstdint>
#include <string>
#include <string_view>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;

using float32 = float;
using float64 = double;

struct FGenericPlatformTypes
{
	using ANSIChar = char;
	using WChar = wchar_t;
	using UTF8Char = char8_t;
	using UTF16Char = char16_t;
	using UTF32Char = char32_t;
};

using ANSIChar = FGenericPlatformTypes::ANSIChar;
using WChar = FGenericPlatformTypes::WChar;
using UTF8Char = FGenericPlatformTypes::UTF8Char;
using UTF16Char = FGenericPlatformTypes::UTF16Char;
using UTF32Char = FGenericPlatformTypes::UTF32Char;

using FANSIString = std::string;
using FU8String = std::u8string;
using FU16String = std::u16string;
using FU32String = std::u32string;

using FANSIStringView = std::string_view;
using FU8StringView = std::u8string_view;
using FU16StringView = std::u16string_view;
using FU32StringView = std::u32string_view;

#define UTF8TEXT(x) u8##x
#define UTF16TEXT(x) u##x
#define UTF32TEXT(x) U##x

// Type CharT will be defined in the platform-specific header

template<typename T>
concept StringType =
	std::is_same_v<T, FANSIString> ||
	std::is_same_v<T, FU8String> ||
	std::is_same_v<T, FU16String> ||
	std::is_same_v<T, FU32String>;

template<typename T>
concept StringViewType =
	std::is_same_v<T, FANSIStringView> ||
	std::is_same_v<T, FU8StringView> ||
	std::is_same_v<T, FU16StringView> ||
	std::is_same_v<T, FU32StringView>;

template<typename T>
concept StringOrStringViewType =
	StringType<T> || StringViewType<T>;

template<StringOrStringViewType T>
inline const char* ToCStr(const T& Str)
{
	return reinterpret_cast<const char*>(Str.data());
}

template<StringOrStringViewType T>
inline FANSIString ToString_ANSI(const T& Str)
{
	return FANSIString(reinterpret_cast<const char*>(Str.data()), Str.size());
}

template<StringOrStringViewType T>
inline FANSIStringView ToStringView_ANSI(const T& Str)
{
	return FANSIStringView(reinterpret_cast<const char*>(Str.data()), Str.size());
}

auto ConvertToStringView_ANSI = [](auto&& arg) -> decltype(auto) {
	using T = std::decay_t<decltype(arg)>;
	if constexpr (std::is_constructible_v<std::u8string_view, T>)
	{
		return std::string_view(
			reinterpret_cast<const char*>(std::u8string_view(arg).data()),
			std::u8string_view(arg).size());
	}
	else
	{
		return std::forward<decltype(arg)>(arg);
	}
};



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

template<typename T>
inline const char* ToCStr(const T& Str)
{
	return reinterpret_cast<const char*>(Str.data());
}

template<typename T>
inline FANSIString ToString_ANSI(const T& Str)
{
	return FANSIString(reinterpret_cast<const char*>(Str.data()), Str.size());
}

template<typename T>
inline FANSIStringView ToStringView_ANSI(const T& Str)
{
	return FANSIStringView(reinterpret_cast<const char*>(Str.data()), Str.size());
}



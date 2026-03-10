#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Misc/Build.h"

namespace Doge
{
	using uint8 = std::uint8_t;
	using uint16 = std::uint16_t;
	using uint32 = std::uint32_t;
	using uint64 = std::uint64_t;

	using int8 = std::int8_t;
	using int16 = std::int16_t;
	using int32 = std::int32_t;
	using int64 = std::int64_t;

	using float32 = float;
	using float64 = double;

	struct FGenericPlatformTypes
	{
		using ANSIChar = char;
		using WChar = wchar_t;
		using UTF8Char = char; // Should be char8_t, but the compiler and other libraries have not fully supported it yet.
		using UTF16Char = char16_t;
		using UTF32Char = char32_t;
	};

	using ANSIChar = FGenericPlatformTypes::ANSIChar;
	using WChar = FGenericPlatformTypes::WChar;
	using U8Char = FGenericPlatformTypes::UTF8Char;
	using U16Char = FGenericPlatformTypes::UTF16Char;
	using U32Char = FGenericPlatformTypes::UTF32Char;

	using FANSIString = std::string;
	using FU8String = std::string; // Should be std::u8string, but the compiler and other libraries have not fully supported it yet.
	using FU16String = std::u16string;
	using FU32String = std::u32string;

	using FANSIStringView = std::string_view;
	using FU8StringView = std::string_view; // Should be std::u8string_view, but the compiler and other libraries have not fully supported it yet.
	using FU16StringView = std::u16string_view;
	using FU32StringView = std::u32string_view;

	#define UTF8TEXT(x) x // Should be u8##x, but the compiler and other libraries have not fully supported it yet.
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

	struct FGenericPlatformMisc
	{
	};

	struct FGenericPlatformLTS
	{
	};
}
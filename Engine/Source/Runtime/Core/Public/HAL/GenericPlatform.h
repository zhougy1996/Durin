#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
		using UTF8Char = char; // Should be char8_t, but the compiler and other libraries have not fully supported it yet.
	};

	using ANSIChar = FGenericPlatformTypes::ANSIChar;
	using U8Char = FGenericPlatformTypes::UTF8Char;

	using FANSIString = std::string;
	using FU8String = std::string; // Should be std::u8string, but the compiler and other libraries have not fully supported it yet.

	using FANSIStringView = std::string_view;
	using FU8StringView = std::string_view; // Should be std::u8string_view, but the compiler and other libraries have not fully supported it yet.

	#define UTF8TEXT(x) x // Should be u8##x, but the compiler and other libraries have not fully supported it yet.
	#define UTF16TEXT(x) u##x
	#define UTF32TEXT(x) U##x

	struct FGenericPlatformMisc
	{
	};

	struct FGenericPlatformLTS
	{
	};
}
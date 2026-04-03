#pragma once

namespace Doge
{
	struct FGenericPlatformTypes
	{
		using ANSIChar = char;
		using UTF8Char = char; // Should be char8_t, but the compiler and other libraries have not fully supported it yet.
	};

	struct FGenericPlatformMisc
	{
	};

	struct FGenericPlatformLTS
	{
	};
}
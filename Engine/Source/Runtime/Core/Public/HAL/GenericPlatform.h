#pragma once

namespace Durin
{
	struct FGenericPlatformTypes
	{
		using ANSIChar = char;
		using UTF8Char = char; // Should be char8_t, but the compiler and other libraries have not fully supported it yet.
	};

	using char8 = FGenericPlatformTypes::UTF8Char;

	struct FGenericPlatformMisc
	{
		static auto EnableUserBinaryDirectoriesSearch() -> void
		{
		}

		static auto AddRuntimeBinaryDirectory(const char* Directory) -> void
		{
			(void)Directory;
		}
	};

	struct FGenericPlatformLTS
	{
	};
}

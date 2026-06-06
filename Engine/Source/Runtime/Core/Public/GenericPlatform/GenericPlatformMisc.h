#pragma once

#include <cstddef>

namespace Durin
{
	struct FGenericPlatformMisc
	{
		static constexpr auto FLibraryPrefix = "";
		static constexpr auto FLibraryExtension = "";

		static auto EnableUserBinaryDirectoriesSearch() -> void
		{
		}

		static auto AddRuntimeBinaryDirectory(const char* Directory) -> void
		{
			(void)Directory;
		}

		static auto Strnicmp(const char* Str1, const char* Str2, size_t Count) -> int
		{
			(void)Str1;
			(void)Str2;
			(void)Count;
			return 0;
		}
	};
}

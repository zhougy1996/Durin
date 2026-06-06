#pragma once

#include "Windows/WindowsPlatform.h"

#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/StringConvert.h"

namespace Durin
{
	struct FWindowsPlatformMisc : public FGenericPlatformMisc
	{
		static constexpr auto FLibraryPrefix = "";
		static constexpr auto FLibraryExtension = ".dll";

		static auto EnableUserBinaryDirectoriesSearch() -> void
		{
			SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
		}

		static auto AddRuntimeBinaryDirectory(const char* Directory) -> void
		{
			AddDllDirectory(String::Utf8ToWide(Directory).c_str());
		}

		static auto LoadLibrary(const std::string& FileName) -> FModuleHandle
		{
			std::wstring WideModuleName(FileName.begin(), FileName.end());
			return ::LoadLibraryW(WideModuleName.c_str());
		}

		static auto FreeLibrary(FModuleHandle ModuleHandle) -> void
		{
			::FreeLibrary(ModuleHandle);
		}

		static auto GetProcAddress(FModuleHandle ModuleHandle, const char* ProcName) -> void*
		{
			return reinterpret_cast<void*>(::GetProcAddress(ModuleHandle, ProcName));
		}

		static auto Prefetch(const void* Ptr) -> void
		{
			_mm_prefetch(static_cast<const char*>(Ptr), _MM_HINT_T0);
		}

		static auto AlignedAlloc(size_t Size, size_t Alignment) -> void*
		{
			return _aligned_malloc(Size, Alignment);
		}

		static auto AlignedRealloc(void* Ptr, size_t NewSize, size_t Alignment) -> void*
		{
			return _aligned_realloc(Ptr, NewSize, Alignment);
		}

		static auto AlignedFree(void* Ptr) -> void
		{
			_aligned_free(Ptr);
		}

		static auto Strnicmp(const char* Str1, const char* Str2, size_t Count) -> int
		{
			return _strnicmp(Str1, Str2, Count);
		}
	};

	using FPlatformMisc = FWindowsPlatformMisc;
}

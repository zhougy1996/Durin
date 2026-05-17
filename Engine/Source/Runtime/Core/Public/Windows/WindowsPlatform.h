#pragma once

#define PLATFORM_HEADER_NAME Windows

#include <string>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"

#include "Misc/CoreTypes.h"
#include "HAL/GenericPlatform.h"

#define DURIN_BUILD_PLATFORM Win64
#define DURIN_BUILD_PLATFORM_STRING "Win64"

#define PLATFORM_LITTLE_ENDIAN 1

#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)

#define FORCEINLINE __forceinline
#define FORCENOINLINE __declspec(noinline)

#define PLATFORM_BREAK() (__nop(), __debugbreak())

#define STR(x) x

namespace Durin
{
	using FModuleHandle = HMODULE;

	struct FWindowsPlatformMisc : public FGenericPlatformMisc
	{
		static constexpr auto FLibraryPrefix = "";
		static constexpr auto FLibraryExtension = ".dll";

		static auto EnableUserBinaryDirectoriesSearch() -> void
		{
			SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
		}

		static FModuleHandle LoadLibrary(const std::string& FileName)
		{
			std::wstring WideModuleName(FileName.begin(), FileName.end());
			return ::LoadLibraryW(WideModuleName.c_str());
		}

		static void FreeLibrary(FModuleHandle ModuleHandle)
		{
			::FreeLibrary(ModuleHandle);
		}

		// GetProcAddress
		static void* GetProcAddress(FModuleHandle ModuleHandle, const char* ProcName)
		{
			return reinterpret_cast<void*>(::GetProcAddress(ModuleHandle, ProcName));
		}

		static void Prefetch(const void* Ptr)
		{
			_mm_prefetch(static_cast<const char*>(Ptr), _MM_HINT_T0);
		}

		static void* AlignedAlloc(size_t Size, size_t Alignment)
		{
			return _aligned_malloc(Size, Alignment);
		}

		static void* AlignedRealloc(void* Ptr, size_t NewSize, size_t Alignment)
		{
			return _aligned_realloc(Ptr, NewSize, Alignment);
		}

		static void AlignedFree(void* Ptr)
		{
			_aligned_free(Ptr);
		}

		static int Strncasecmp(const char* Str1, const char* Str2, size_t Count)
		{
			return _strnicmp(Str1, Str2, Count);
		}

	};


	struct FWindowsPlatformLTS : public FWindowsPlatformMisc
	{
		static FORCEINLINE auto GetCurrentThreadId() -> uint32
		{
			return ::GetCurrentThreadId();
		}
	};

	using FPlatformMisc = FWindowsPlatformMisc;
	using FPlatformLTS = FWindowsPlatformLTS;
}

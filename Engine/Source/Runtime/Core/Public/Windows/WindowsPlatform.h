#pragma once

#include <string>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"

#include "HAL/GenericPlatform.h"

#define PLATFORM_LITTLE_ENDIAN 1

#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)

#define FORCEINLINE __forceinline
#define FORCENOINLINE __declspec(noinline)

#define PLATFORM_BREAK() (/*__nop(), */__debugbreak())

// Define a macro to convert string literals
#define STR(x) x

#ifdef _DEBUG
	#define DOGE_VISUALIZERS_HELPERS
#endif // _DEBUG

#pragma warning(disable : 4251)

namespace Doge
{
	using FModuleHandle = HMODULE;

	using CharT = U8Char;
	using FString = FU8String;
	using FStringView = FU8StringView;

	struct FWindowsPlatformMisc : public FGenericPlatformMisc
	{
		static constexpr auto FLibraryPrefix = "";
		static constexpr auto FLibraryExtension = ".dll";

		static FModuleHandle LoadLibrary(const FString& FileName)
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
			return ::GetProcAddress(ModuleHandle, ProcName);
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

	using FPlatformMisc = FWindowsPlatformMisc;
}

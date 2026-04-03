#pragma once

#define PLATFORM_HEADER_NAME MacOS

#include <cassert>
#include <malloc/malloc.h>
#include <dlfcn.h>

#include "HAL/GenericPlatform.h"

#define PLATFORM_LITTLE_ENDIAN 1

#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))

#define FORCEINLINE __attribute__((always_inline)) inline
#define FORCENOINLINE __attribute__((noinline))

#define PLATFORM_BREAK() __builtin_trap()

#define STR(x) x

namespace Doge
{
	using CharT = char;

	// Define a macro to convert string literals

	using FModuleHandle = void*;

	struct FMacOSPlatformMisc : public FGenericPlatformMisc
	{
		static constexpr auto FLibraryPrefix = "lib";
		static constexpr auto FLibraryExtension = ".dylib";

		static FModuleHandle LoadLibrary(const std::string& FileName)
		{
			return dlopen(FileName.c_str(), RTLD_NOW | RTLD_LOCAL);
		}

		static void FreeLibrary(FModuleHandle InModuleHandle)
		{
			dlclose(InModuleHandle);
		}

		// GetProcAddress
		static void* GetProcAddress(FModuleHandle ModuleHandle, const char* ProcName)
		{
			return dlsym(ModuleHandle, ProcName);
		}

		static void Prefetch(const void* Ptr)
		{
			__builtin_prefetch(Ptr, 0, 3);
		}

		/**
		 * Allocates aligned memory using posix_memalign.
		 */
		static void* AlignedAlloc(size_t Size, size_t Alignment)
		{
			if (Size == 0 || Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
			{
				return nullptr;
			}

			constexpr size_t MinAlignment = sizeof(void*);
			size_t RealAlignment = (Alignment < MinAlignment) ? MinAlignment : Alignment;

			void* Ptr = nullptr;
			int Result = posix_memalign(&Ptr, RealAlignment, Size);
			if (Result != 0)
			{
				return nullptr;
			}
			return Ptr;
		}

		/**
		 * Reallocates aligned memory.
		 * Since there is no native realloc_aligned on macOS, we must manually copy data.
		 */
		static void* AlignedRealloc(void* Ptr, size_t NewSize, size_t Alignment)
		{
			if (!Ptr)
			{
				return AlignedAlloc(NewSize, Alignment);
			}

			if (NewSize == 0)
			{
				free(Ptr);
				return nullptr;
			}

			// Allocate new block with required alignment
			void* NewPtr = AlignedAlloc(NewSize, Alignment);

			if (NewPtr)
			{
				// Use malloc_size to determine the actual allocated size of the old block.
				// This prevents reading past the end of the buffer (Buffer Over-read).
				size_t OldSize = malloc_size(Ptr);

				// Only copy the minimum of the two sizes
				size_t CopySize = (OldSize < NewSize) ? OldSize : NewSize;

				memcpy(NewPtr, Ptr, CopySize);
				free(Ptr);
			}

			return NewPtr;
		}

		static void AlignedFree(void* Ptr)
		{
			free(Ptr);
		}

		// strncasecmp is a POSIX function that compares two strings ignoring case, up to a specified number of characters.
		static int Strncasecmp(const char* Str1, const char* Str2, size_t Count)
		{
			return strncasecmp(Str1, Str2, Count);
		}

	};

	using FPlatformMisc = FMacOSPlatformMisc;
}
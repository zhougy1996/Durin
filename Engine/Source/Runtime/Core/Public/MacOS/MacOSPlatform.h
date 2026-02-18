#pragma once

#include "HAL/GenericPlatform.h"

#define PLATFORM_LITTLE_ENDIAN 1

#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))

#define FORCEINLINE __attribute__((always_inline))
#define FORCENOINLINE __attribute__((noinline))

#define PLATFORM_BREAK() __builtin_trap()

using CharT = UTF8Char;
using FString = FU8String;
using FStringView = FU8StringView;

// Define a macro to convert string literals
#define STR(x) x

struct FMacOSPlatformMisc : public FGenericPlatformMisc
{
	static void Prefetch(const void* Ptr)
	{
		__builtin_prefetch(Ptr, 0, 3);
	}
};

using FPlatformMisc = FMacOSPlatformMisc;


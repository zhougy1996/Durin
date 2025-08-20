#pragma once

#include <string>

#define NOMINMAX
#include "Windows.h"

#include "HAL/GenericPlatform.h"

#define PLATFORM_LITTLE_ENDIAN 1

#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)

#define FORCEINLINE __forceinline
#define FORCENOINLINE __declspec(noinline)

#define PLATFORM_BREAK() (/*__nop(), */__debugbreak())

using CharT = UTF8Char;
using FString = FU8String;
using FStringView = FU8StringView;

using FStringName = FString; // temporary replacement for FName, which is not defined here

// Define a macro to convert string literals
#define STR(x) x

struct FWindowsPlatformMisc : public FGenericPlatformMisc
{
	static void Prefetch(const void* Ptr)
	{
		_mm_prefetch(static_cast<const char*>(Ptr), _MM_HINT_T0);
	}
};

using FPlatformMisc = FWindowsPlatformMisc;

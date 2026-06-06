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
}

#pragma once

namespace Durin
{
	struct FGenericPlatformTypes
	{
		using ANSIChar = char;
		using UTF8Char = char; // Should be char8_t, but the compiler and other libraries have not fully supported it yet.
	};

	using char8 = FGenericPlatformTypes::UTF8Char;
}

#include "GenericPlatform/GenericPlatformMisc.h"
#include "GenericPlatform/GenericPlatformLTS.h"

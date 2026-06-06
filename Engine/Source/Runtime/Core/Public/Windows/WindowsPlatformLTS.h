#pragma once

#include "Windows/WindowsPlatform.h"

#include "GenericPlatform/GenericPlatformLTS.h"

namespace Durin
{
	struct FWindowsPlatformLTS : public FGenericPlatformLTS
	{
		static FORCEINLINE auto GetCurrentThreadId() -> uint32
		{
			return ::GetCurrentThreadId();
		}
	};

	using FPlatformLTS = FWindowsPlatformLTS;
}

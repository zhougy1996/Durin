#pragma once

#include <pthread.h>

#include "MacOS/MacOSPlatform.h"

#include "GenericPlatform/GenericPlatformLTS.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	struct FMacOSPlatformLTS : public FGenericPlatformLTS
	{
		static auto GetCurrentThreadId() -> uint32
		{
			uint64 ThreadId = 0;
			pthread_threadid_np(nullptr, &ThreadId);
			return static_cast<uint32>(ThreadId);
		}
	};

	using FPlatformLTS = FMacOSPlatformLTS;
}

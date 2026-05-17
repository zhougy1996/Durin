#pragma once

#include "CoreAPI.h"

#include "GenericPlatform/GenericPlatformProcess.h"

namespace Durin
{
	struct FWindowsPlatformProcess : public FGenericPlatformProcess
	{
		static CORE_API auto ExecutablePath() -> const char*;
	};

	using FPlatformProcess = FWindowsPlatformProcess;
}
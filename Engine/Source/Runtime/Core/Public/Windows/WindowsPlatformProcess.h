#pragma once

#include "CoreAPI.h"

#include "GenericPlatform/GenericPlatformProcess.h"

namespace Durin
{
	struct FWindowsPlatformProcess : public FGenericPlatformProcess
	{
		static CORE_API auto ExecutablePath() -> const char*;
		static CORE_API auto CurrentProcessId() -> uint32;
		static CORE_API auto WaitForProcessExit(uint32 ProcessId, std::string* OutError = nullptr) -> bool;
		static CORE_API auto LaunchProcess(std::string_view Executable, std::string_view Arguments, std::string* OutError = nullptr) -> bool;
	};

	using FPlatformProcess = FWindowsPlatformProcess;
}

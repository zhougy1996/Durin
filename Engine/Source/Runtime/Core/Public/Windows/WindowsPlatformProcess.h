#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "CoreAPI.h"

#include "GenericPlatform/GenericPlatformProcess.h"

namespace Durin
{
	struct FWindowsPlatformProcess : public FGenericPlatformProcess
	{
		static CORE_API auto ExecutablePath() -> const char*;
		static CORE_API auto CurrentProcessId() -> uint32;
		[[nodiscard]] static CORE_API auto WaitForProcessExit(uint32 ProcessId) -> std::expected<void, FPlatformProcessError>;
		// Requires an absolute UTF-8 executable path. Success reports creation, not child exit.
		[[nodiscard]] static CORE_API auto LaunchProcess(std::string_view Executable, const FProcessLaunchOptions& Options = {}) -> std::expected<void, FPlatformProcessError>;
		// Same launch options; waits for exit. Nonzero exit codes are values, not errors.
		[[nodiscard]] static CORE_API auto ExecuteProcess(
			std::string_view Executable,
			const FProcessLaunchOptions& Options = {}) -> std::expected<int32, FPlatformProcessError>;
		[[nodiscard]] static CORE_API auto OpenPath(std::string_view Path) -> std::expected<void, FPlatformProcessError>;
	};

	using FPlatformProcess = FWindowsPlatformProcess;
}

#pragma once

#include "CoreAPI.h"

#include "GenericPlatform/GenericPlatformProcess.h"

namespace Durin
{
	struct FMacOSPlatformProcess : public FGenericPlatformProcess
	{
		static CORE_API auto ExecutablePath() -> const char*;
		static CORE_API auto CurrentProcessId() -> uint32;
		[[nodiscard]] static CORE_API auto WaitForProcessExit(
			uint32 ProcessId) -> std::expected<void, FPlatformProcessError>;
		[[nodiscard]] static CORE_API auto LaunchProcess(
			std::string_view Executable,
			std::string_view Arguments) -> std::expected<void, FPlatformProcessError>;
		// Nonzero child exit codes are values; launch/wait failures are errors.
		[[nodiscard]] static CORE_API auto ExecuteProcess(
			std::string_view Executable,
			std::string_view Arguments) -> std::expected<int32, FPlatformProcessError>;
		[[nodiscard]] static CORE_API auto OpenPath(
			std::string_view Path) -> std::expected<void, FPlatformProcessError>;
	};

	using FPlatformProcess = FMacOSPlatformProcess;
}

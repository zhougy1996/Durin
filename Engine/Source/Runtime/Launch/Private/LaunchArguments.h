#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Owns normal host choices that are safe to pass into engine startup.
	struct FEngineStartupParams
	{
		std::optional<std::string> ProjectFile;
		bool bOpenProjectBrowser = false;
		bool bSuppressWindowDisplay = false;
	};

	// Owns process coordination that must complete before engine startup.
	struct FLaunchProcessCoordinationRequest
	{
		std::optional<uint32> WaitForProcessId;
	};

	// Owns optional startup-command publication and its ordered arguments.
	struct FLaunchStartupCommandRequest
	{
		std::optional<std::string> Name;
		std::vector<std::string> Arguments;
	};

	// Owns automation that controls the application run loop.
	struct FLaunchAutomationRequest
	{
		std::optional<uint64> ExitAfterTicks;
		FLaunchStartupCommandRequest StartupCommand;
	};

	// Owns isolated process-entry crash qualification configuration.
	struct FLaunchCrashTestRequest
	{
		std::optional<std::string> NativeCrashFixture;
		std::optional<std::string> NativeCrashSavedRoot;
		bool bDisableNativeCrashDump = false;
		bool bForceNativeCrashCollision = false;
		bool bFaultNativeCrashWriter = false;
	};

	// Owns the complete validated process request without retaining argv storage.
	struct FLaunchRequest
	{
		FLaunchProcessCoordinationRequest ProcessCoordination;
		FEngineStartupParams Host;
		FLaunchAutomationRequest Automation;
		FLaunchCrashTestRequest CrashTest;
	};

	// Classifies a rejected command line with actionable user-facing text.
	struct FLaunchArgumentError
	{
		int ExitCode = 2;
		std::string Option;
		std::string Message;
	};

	// Contains either one fully owned request or one deterministic contract error.
	struct FLaunchArgumentResult
	{
		std::optional<FLaunchRequest> Request;
		std::optional<FLaunchArgumentError> Error;

		explicit operator bool() const { return Request.has_value(); }
	};

	auto ParseLaunchArguments(std::span<const std::string_view> Arguments)
		-> FLaunchArgumentResult;
}

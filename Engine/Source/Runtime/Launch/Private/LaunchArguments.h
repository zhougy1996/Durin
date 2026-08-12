#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Identifies the explicit lifecycle boundary used by a native-crash fixture.
	enum class ENativeCrashPhase : uint8
	{
		ProcessEntry,
		PreInitialization,
		LoggerRunning,
		Running,
		ObjectCollection
	};

	// Owns normal host choices that are safe to pass into engine startup.
	struct FLaunchHostRequest
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

	// Owns opt-in qualification and native-crash configuration.
	struct FLaunchDiagnosticsRequest
	{
		bool bRunTaskSchedulerLifecycleSmoke = false;
		bool bRunEngineAssetServiceLifecycleSmoke = false;
		bool bRunEditorPIELifecycleSmoke = false;
		bool bRunNativeGameplayLifecycleSmoke = false;
		std::optional<std::string> NativeCrashFixture;
		std::optional<std::string> NativeCrashSavedRoot;
		std::optional<ENativeCrashPhase> NativeCrashPhase;
		bool bDisableNativeCrashDump = false;
		bool bForceNativeCrashCollision = false;
		bool bFillNativeCrashLogGap = false;
		bool bFaultNativeCrashWriter = false;
	};

	// Owns the complete validated process request without retaining argv storage.
	struct FLaunchRequest
	{
		FLaunchProcessCoordinationRequest ProcessCoordination;
		FLaunchHostRequest Host;
		FLaunchAutomationRequest Automation;
		FLaunchDiagnosticsRequest Diagnostics;
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

#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Describes the physical runtime storage roots consumed by one preparation pass.
	struct FLaunchRuntimeStoragePaths
	{
		std::filesystem::path LaunchDirectory;
		std::filesystem::path SavedDirectory;
		std::filesystem::path ConfigDirectory;
		std::filesystem::path LogDirectory;
	};

	// Returns the selected application configuration and pass-local migration diagnostics.
	struct FLaunchRuntimeStorageResult
	{
		std::filesystem::path AppConfigPath;
		std::vector<std::string> Warnings;
	};

	// Test-only controls for deterministic fallback qualification of private storage policy.
	struct FLaunchRuntimeStorageTestOptions
	{
		bool bForceLegacyFileRenameFailure = false;
	};

	auto PrepareLaunchRuntimeStorage(
		const FLaunchRuntimeStoragePaths& Paths,
		std::string_view AppConfigFileName,
		const FLaunchRuntimeStorageTestOptions& TestOptions = {})
		-> FLaunchRuntimeStorageResult;
	auto PrepareLaunchRuntimeStorage() -> FLaunchRuntimeStorageResult;
}

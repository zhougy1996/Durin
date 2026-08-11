#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Describes the physical runtime storage roots consumed by one preparation pass.
	struct FRuntimeStoragePaths
	{
		std::filesystem::path LaunchDirectory;
		std::filesystem::path SavedDirectory;
		std::filesystem::path ConfigDirectory;
		std::filesystem::path LogDirectory;
	};

	// Returns the selected application configuration and pass-local migration diagnostics.
	struct FRuntimeStoragePreparationResult
	{
		std::filesystem::path AppConfigPath;
		std::vector<std::string> Warnings;
	};

	// Test-only controls for deterministic fallback qualification of private storage policy.
	struct FRuntimeStorageTestOptions
	{
		bool bForceLegacyFileRenameFailure = false;
	};

	auto PrepareRuntimeStorage(
		const FRuntimeStoragePaths& Paths,
		std::string_view AppConfigFileName,
		const FRuntimeStorageTestOptions& TestOptions = {})
		-> FRuntimeStoragePreparationResult;
	auto PrepareRuntimeStorage() -> FRuntimeStoragePreparationResult;
}

#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Describes the physical runtime storage roots consumed by one preparation pass.
	struct FRuntimeStoragePaths
	{
		std::filesystem::path SavedDirectory;
		std::filesystem::path ConfigDirectory;
		std::filesystem::path LogDirectory;
		std::filesystem::path AppConfigTemplatePath;
	};

	// Returns the canonical application configuration and pass-local preparation diagnostics.
	struct FRuntimeStoragePreparationResult
	{
		std::filesystem::path AppConfigPath;
		std::vector<std::string> Warnings;
	};

	auto PrepareRuntimeStorage(
		const FRuntimeStoragePaths& Paths,
		std::string_view AppConfigFileName)
		-> FRuntimeStoragePreparationResult;
	auto PrepareRuntimeStorage() -> FRuntimeStoragePreparationResult;
}

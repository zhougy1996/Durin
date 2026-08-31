#include "RuntimeStorage.h"

#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view AppConfigFileName = DURIN_RUNTIME_VARIANT ".yaml";

		auto AddFilesystemWarning(
			std::vector<std::string>& Warnings,
			std::string_view Operation,
			const std::filesystem::path& Path,
			const std::error_code& Error) -> void
		{
			Warnings.push_back(std::format(
				"Could not {} '{}': {}", Operation, Path.string(), Error.message()));
		}

		auto PathExists(
			const std::filesystem::path& Path,
			std::vector<std::string>& Warnings) -> bool
		{
			std::error_code Error;
			const bool bExists = std::filesystem::exists(Path, Error);
			if (Error) AddFilesystemWarning(Warnings, "inspect runtime path", Path, Error);
			return bExists;
		}

		auto CreateAppConfigIfMissing(
			const FRuntimeStoragePaths& Paths,
			const std::filesystem::path& AppConfigPath,
			std::vector<std::string>& Warnings) -> void
		{
			if (PathExists(AppConfigPath, Warnings)) return;

			std::error_code Error;
			std::filesystem::copy_file(
				Paths.AppConfigTemplatePath,
				AppConfigPath,
				std::filesystem::copy_options::none,
				Error);
			if (!Error) return;

			std::error_code InspectError;
			if (std::filesystem::exists(AppConfigPath, InspectError)) return;
			Warnings.push_back(std::format(
				"Could not create application config '{}' from template '{}': {}",
				AppConfigPath.string(),
				Paths.AppConfigTemplatePath.string(),
				Error.message()));
		}
	}

	auto PrepareRuntimeStorage(
		const FRuntimeStoragePaths& Paths,
		std::string_view InAppConfigFileName)
		-> FRuntimeStoragePreparationResult
	{
		FRuntimeStoragePreparationResult Result;
		Result.AppConfigPath = Paths.ConfigDirectory / InAppConfigFileName;
		std::error_code Error;
		std::filesystem::create_directories(Paths.SavedDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime saved directory", Paths.SavedDirectory, Error);
			return Result;
		}

		Error.clear();
		std::filesystem::create_directories(Paths.ConfigDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime config directory", Paths.ConfigDirectory, Error);
			return Result;
		}
		Error.clear();
		std::filesystem::create_directories(Paths.LogDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime log directory", Paths.LogDirectory, Error);
		}

		CreateAppConfigIfMissing(Paths, Result.AppConfigPath, Result.Warnings);
		return Result;
	}

	auto PrepareRuntimeStorage() -> FRuntimeStoragePreparationResult
	{
		return PrepareRuntimeStorage({
			.SavedDirectory = FPaths::LaunchSavedDir(),
			.ConfigDirectory = FPaths::LaunchConfigsDir(),
			.LogDirectory = FPaths::LaunchLogsDir(),
			.AppConfigTemplatePath = std::filesystem::path(FPaths::LaunchDir())
				/ "Templates" / std::format("TP_{}", AppConfigFileName)},
			AppConfigFileName);
	}
}

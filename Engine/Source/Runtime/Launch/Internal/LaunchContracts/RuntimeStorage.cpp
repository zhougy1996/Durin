#include "LaunchContracts/RuntimeStorage.h"

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

		auto MigrateLegacyRuntimeFile(
			const FRuntimeStoragePaths& Paths,
			std::string_view FileName,
			const FRuntimeStorageTestOptions& TestOptions,
			std::vector<std::string>& Warnings) -> void
		{
			const std::filesystem::path LegacyPath = Paths.LaunchDirectory / FileName;
			const std::filesystem::path SavedPath = Paths.ConfigDirectory / FileName;
			if (!PathExists(LegacyPath, Warnings) || PathExists(SavedPath, Warnings)) return;

			std::error_code Error;
			if (TestOptions.bForceLegacyFileRenameFailure)
				Error = std::make_error_code(std::errc::cross_device_link);
			else
				std::filesystem::rename(LegacyPath, SavedPath, Error);
			if (!Error) return;

			Error.clear();
			std::filesystem::copy_file(
				LegacyPath, SavedPath, std::filesystem::copy_options::none, Error);
			if (!Error)
			{
				std::error_code RemoveError;
				std::filesystem::remove(LegacyPath, RemoveError);
				return;
			}
			Warnings.push_back(std::format(
				"Could not migrate legacy runtime file '{}' to '{}': {}",
				LegacyPath.string(), SavedPath.string(), Error.message()));
		}
	}

	auto PrepareRuntimeStorage(
		const FRuntimeStoragePaths& Paths,
		std::string_view InAppConfigFileName,
		const FRuntimeStorageTestOptions& TestOptions)
		-> FRuntimeStoragePreparationResult
	{
		FRuntimeStoragePreparationResult Result;
		std::error_code Error;
		std::filesystem::create_directories(Paths.SavedDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime saved directory", Paths.SavedDirectory, Error);
			Result.AppConfigPath = Paths.LaunchDirectory / InAppConfigFileName;
			return Result;
		}

		const std::filesystem::path LegacyLogs = Paths.LaunchDirectory / "Logs";
		if (PathExists(LegacyLogs, Result.Warnings)
			&& !PathExists(Paths.LogDirectory, Result.Warnings))
		{
			Error.clear();
			std::filesystem::rename(LegacyLogs, Paths.LogDirectory, Error);
			if (Error)
			{
				Result.Warnings.push_back(std::format(
					"Could not migrate legacy log directory '{}' to '{}': {}",
					LegacyLogs.string(), Paths.LogDirectory.string(), Error.message()));
			}
		}

		Error.clear();
		std::filesystem::create_directories(Paths.ConfigDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime config directory", Paths.ConfigDirectory, Error);
			Result.AppConfigPath = Paths.LaunchDirectory / InAppConfigFileName;
			return Result;
		}
		Error.clear();
		std::filesystem::create_directories(Paths.LogDirectory, Error);
		if (Error)
		{
			AddFilesystemWarning(
				Result.Warnings, "create runtime log directory", Paths.LogDirectory, Error);
		}

		MigrateLegacyRuntimeFile(Paths, InAppConfigFileName, TestOptions, Result.Warnings);

		const std::filesystem::path SavedAppConfig =
			Paths.ConfigDirectory / InAppConfigFileName;
		Result.AppConfigPath = PathExists(SavedAppConfig, Result.Warnings)
			? SavedAppConfig : Paths.LaunchDirectory / InAppConfigFileName;
		return Result;
	}

	auto PrepareRuntimeStorage() -> FRuntimeStoragePreparationResult
	{
		return PrepareRuntimeStorage({
			.LaunchDirectory = FPaths::LaunchDir(),
			.SavedDirectory = FPaths::LaunchSavedDir(),
			.ConfigDirectory = FPaths::LaunchConfigsDir(),
			.LogDirectory = FPaths::LaunchLogsDir()}, AppConfigFileName);
	}
}

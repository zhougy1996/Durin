#include "RuntimeStorage.h"

#include "Misc/Paths.h"
#include "Misc/RuntimePaths.h"
#include "Misc/FileHelper.h"

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
            if (Paths.bUserOverridesOnly)
            {
                FByteBuffer Bytes;
                if (Paths.LegacyUserConfig.empty()) { const std::string_view Empty="{}\n"; const auto View=std::as_bytes(std::span(Empty.data(),Empty.size())); Bytes.assign(View.begin(),View.end()); }
                else
                {
                    auto Legacy=FFileHelper::LoadFileToArray(Paths.LegacyUserConfig,{.MaximumBytes=1024*1024});
                    if (!Legacy) { Warnings.push_back("Cannot read explicit user config migration: "+Legacy.error().GetMessage()); return; }
                    Bytes=std::move(*Legacy);
                }
                const auto Saved=FFileHelper::SaveArrayToNewFile(Bytes,AppConfigPath);
                if (!Saved && Saved.error().NativeError!=std::errc::file_exists)
                    Warnings.push_back("Could not create user-only overrides: "+Saved.error().GetMessage());
                return;
            }

			std::error_code Error;
			std::filesystem::copy_file(
				Paths.bUserOverridesOnly ? Paths.LegacyUserConfig : Paths.AppConfigTemplatePath,
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
        if (Paths.bUserOverridesOnly)
        {
            auto Saved=Paths.SavedDirectory.lexically_normal();
            if (Saved.filename().empty()) Saved=Saved.parent_path();
            const auto Root=std::filesystem::weakly_canonical(Saved.parent_path());
            for (const auto& Candidate:{Paths.SavedDirectory,Paths.ConfigDirectory,Paths.LogDirectory,Paths.CacheDirectory})
            {
                if (Candidate.empty()) continue;
                std::filesystem::path Resolved; std::error_code Error;
                auto Normalized=Candidate.lexically_normal(); if (Normalized.filename().empty()) Normalized=Normalized.parent_path();
                if (!FPaths::TryResolveContainedPath(Normalized,Root,Resolved,Error))
                { Result.Warnings.push_back("User storage path escapes application data root: "+Candidate.string()); return Result; }
            }
        }
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
        if (!Paths.CacheDirectory.empty())
        {
            Error.clear(); std::filesystem::create_directories(Paths.CacheDirectory,Error);
            if (Error) AddFilesystemWarning(Result.Warnings,"create user cache directory",Paths.CacheDirectory,Error);
        }
		return Result;
	}

	auto PrepareRuntimeStorage() -> FRuntimeStoragePreparationResult
	{
		return PrepareRuntimeStorage({
			.SavedDirectory = FPaths::LaunchSavedDir(),
			.ConfigDirectory = FPaths::LaunchConfigsDir(),
			.LogDirectory = FPaths::LaunchLogsDir(),
			.AppConfigTemplatePath = RuntimePathBinding("Defaults").empty() ? std::filesystem::path(FPaths::LaunchDir())
				/ "Templates" / std::format("TP_{}", AppConfigFileName) : std::filesystem::path(RuntimePathBinding("Defaults")),
            .bUserOverridesOnly = GetRuntimePathContext() && GetRuntimePathContext()->bStaged,
            .LegacyUserConfig = RuntimePathBinding("LegacyUserConfig"),
            .CacheDirectory = RuntimePathBinding("Cache")},
			AppConfigFileName);
	}
}

#include "AssetCompatibility.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Level.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include <csignal>
#include <iostream>

namespace
{
	std::atomic_bool GCancelled = false;

	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	auto ParseProject(int ArgC, char** ArgV, std::string& OutProject, std::string& OutError) -> bool
	{
		for (int Index = 1; Index < ArgC; ++Index)
		{
			const std::string_view Argument = ArgV[Index];
			if (Argument.starts_with("--project="))
			{
				if (!OutProject.empty())
				{
					OutError = "--project may be specified only once.";
					return false;
				}
				OutProject = Argument.substr(std::string_view("--project=").size());
			}
			else if (Argument != "--format=json")
			{
				OutError = std::format("Unknown argument: {}", Argument);
				return false;
			}
		}
		if (OutProject.empty())
		{
			OutError = "--project=<path-to-project.dproject> is required.";
			return false;
		}
		return true;
	}

	auto SnapshotMountedPackages(
		std::vector<Durin::Asset::FAssetPackageCompatibilityProbeInput>& OutInputs,
		std::string& OutError) -> bool
	{
		for (const Durin::PathUtilities::FMountPoint& Mount : Durin::PathUtilities::GetRegisteredMountPoints())
		{
			if (GCancelled.load(std::memory_order_relaxed)) return false;
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path ContentRoot = Mount.GetContentDir();
			std::error_code Error;
			if (!std::filesystem::exists(ContentRoot, Error))
			{
				if (Error)
				{
					OutError = std::format("Failed to inspect mount {}: {}", Mount.VirtualRoot, Error.message());
					return false;
				}
				continue;
			}
			std::filesystem::recursive_directory_iterator It(
				ContentRoot, std::filesystem::directory_options::skip_permission_denied, Error);
			const std::filesystem::recursive_directory_iterator End;
			while (It != End)
			{
				if (GCancelled.load(std::memory_order_relaxed)) return false;
				if (Error)
				{
					OutError = std::format("Failed to enumerate mount {}: {}", Mount.VirtualRoot, Error.message());
					return false;
				}
				std::error_code FileError;
				if (It->is_regular_file(FileError) && It->path().extension() == ".dasset")
				{
					std::filesystem::path Relative = std::filesystem::relative(It->path(), ContentRoot, FileError);
					if (FileError)
					{
						OutError = std::format("Failed to classify package '{}': {}", It->path().generic_string(), FileError.message());
						return false;
					}
					Relative.replace_extension();
					Durin::FAssetPath PackagePath;
					std::string PathError;
					if (!Durin::FAssetPath::TryCreate(Mount.VirtualRoot + Relative.generic_string(), PackagePath, &PathError))
					{
						OutError = std::format("Invalid mounted package path '{}': {}", It->path().generic_string(), PathError);
						return false;
					}
					const auto LastWriteTime = It->last_write_time(FileError);
					const auto FileSize = It->file_size(FileError);
					if (FileError)
					{
						OutError = std::format("Failed to fingerprint package '{}': {}", It->path().generic_string(), FileError.message());
						return false;
					}
					OutInputs.push_back({
						.PackagePath = std::move(PackagePath),
						.PhysicalPath = It->path().generic_string(),
						.ExpectedFileSize = FileSize,
						.ExpectedLastWriteTimeTicks = Durin::DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
				}
				It.increment(Error);
			}
		}
		std::ranges::sort(OutInputs, [](const auto& Left, const auto& Right) {
			return Left.PackagePath.GetView() < Right.PackagePath.GetView();
		});
		return true;
	}
}

int main(int ArgC, char** ArgV)
{
	std::signal(SIGINT, HandleInterrupt);
	std::string Project;
	std::string Error;
	if (!ParseProject(ArgC, ArgV, Project, Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 2;
	}

	Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
	Durin::GIsGameThreadIdInitialized = true;
	Durin::FPlatformMisc::EnableUserBinaryDirectoriesSearch();
	Durin::FNameInit();
	if (!Durin::InitializeCurrentProject({.RequestedProjectFile = Project}, &Error)
		|| !Durin::PathUtilities::InitDefaultMountPoints(&Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::DObjectInit();
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
	const Durin::Asset::FReflectionCompatibilityCatalog Catalog =
		Durin::Asset::FReflectionCompatibilityCatalog::Capture();

	std::vector<Durin::Asset::FAssetPackageCompatibilityProbeInput> Inputs;
	if (!SnapshotMountedPackages(Inputs, Error))
	{
		if (GCancelled.load(std::memory_order_relaxed)) return 130;
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	std::vector<Durin::Asset::FAssetPackageCompatibilityRecord> Records;
	Records.reserve(Inputs.size());
	for (const auto& Input : Inputs)
	{
		if (GCancelled.load(std::memory_order_relaxed)) return 130;
		auto Result = Durin::Asset::ProbeAssetPackageCompatibility(
			Input, Catalog, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (Result.Status == Durin::Asset::EAssetCompatibilityProbeStatus::Cancelled) return 130;
		if (Result.Record) Records.push_back(std::move(*Result.Record));
	}
	std::cout << Durin::Asset::SerializeAssetCompatibilityReportV1(Records) << '\n';
	return 0;
}

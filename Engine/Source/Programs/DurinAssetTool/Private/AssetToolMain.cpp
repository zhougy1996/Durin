#include "AssetCompatibility.h"
#include "AssetMigration.h"
#include "ImportRecord.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Level.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include <csignal>
#include <iostream>

namespace
{
	std::atomic_bool GCancelled = false;

	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	struct FOptions
	{
		std::string Project;
		bool bMigrate = false;
		bool bApply = false;
		std::vector<std::string> Mounts;
		std::vector<std::string> Packages;
	};

	auto ParseOptions(int ArgC, char** ArgV, FOptions& OutOptions, std::string& OutError) -> bool
	{
		for (int Index = 1; Index < ArgC; ++Index)
		{
			const std::string_view Argument = ArgV[Index];
			if (Argument.starts_with("--project="))
			{
				if (!OutOptions.Project.empty())
				{
					OutError = "--project may be specified only once.";
					return false;
				}
				OutOptions.Project = Argument.substr(std::string_view("--project=").size());
			}
			else if (Argument == "--operation=migrate") OutOptions.bMigrate = true;
			else if (Argument == "--apply") OutOptions.bApply = true;
			else if (Argument.starts_with("--mount=")) OutOptions.Mounts.emplace_back(Argument.substr(std::string_view("--mount=").size()));
			else if (Argument.starts_with("--package=")) OutOptions.Packages.emplace_back(Argument.substr(std::string_view("--package=").size()));
			else if (Argument != "--format=json")
			{
				OutError = std::format("Unknown argument: {}", Argument);
				return false;
			}
		}
		if (OutOptions.Project.empty())
		{
			OutError = "--project=<path-to-project.dproject> is required.";
			return false;
		}
		if (OutOptions.bApply && !OutOptions.bMigrate)
		{
			OutError = "--apply requires --operation=migrate.";
			return false;
		}
		return true;
	}
}

int main(int ArgC, char** ArgV)
{
	std::signal(SIGINT, HandleInterrupt);
	FOptions Options;
	std::string Error;
	if (!ParseOptions(ArgC, ArgV, Options, Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 2;
	}

	Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
	Durin::GIsGameThreadIdInitialized = true;
	Durin::FPlatformMisc::EnableUserBinaryDirectoriesSearch();
	Durin::FNameInit();
	if (!Durin::InitializeCurrentProject({.RequestedProjectFile = Options.Project}, &Error)
		|| !Durin::PathUtilities::InitDefaultMountPoints(&Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::DObjectInit();
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
	(void)Durin::AssetImport::DImportRecord::StaticClass(); // AssetImport packages are part of the authored corpus.
	if (Options.bApply && !Durin::Asset::RecoverInterruptedAssetMigrations(Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	const Durin::Asset::FReflectionCompatibilityCatalog Catalog =
		Durin::Asset::FReflectionCompatibilityCatalog::Capture();
	Durin::Asset::FAssetPackageDiscoverySnapshot Snapshot =
		Durin::Asset::CaptureMountedAssetPackageSnapshot(
			[] { return GCancelled.load(std::memory_order_relaxed); });
	if (Snapshot.Status == Durin::Asset::EAssetPackageSnapshotStatus::Cancelled) return 130;
	if (Snapshot.Status == Durin::Asset::EAssetPackageSnapshotStatus::Failed)
	{
		std::cerr << "Error: " << Snapshot.Error << '\n';
		return 1;
	}

	std::vector<Durin::Asset::FAssetPackageCompatibilityRecord> Records;
	Records.reserve(Snapshot.Packages.size());
	for (const auto& Input : Snapshot.Packages)
	{
		if (GCancelled.load(std::memory_order_relaxed)) return 130;
		auto Result = Durin::Asset::ProbeAssetPackageCompatibility(
			Input, Catalog, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (Result.Status == Durin::Asset::EAssetCompatibilityProbeStatus::Cancelled) return 130;
		if (Result.Record) Records.push_back(std::move(*Result.Record));
	}
	if (!Options.bMigrate)
	{
		std::cout << Durin::Asset::SerializeAssetCompatibilityReportV1(Records) << '\n';
		return 0;
	}

	Durin::Asset::FAssetMigrationRegistry Registry;
	if (!Durin::Asset::RegisterBuiltInAssetMigrations(Registry, Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::Asset::FAssetMigrationSelection Selection{.Mounts = std::move(Options.Mounts)};
	for (const std::string& Mount : Selection.Mounts)
	{
		if (Mount.size() < 2 || Mount.front() != '/' || Mount.back() == '/' || Mount.find("//") != std::string::npos
			|| Mount.find('\\') != std::string::npos)
		{
			std::cerr << "Error: invalid --mount value '" << Mount << "'. Expected a virtual root such as /Engine.\n";
			return 2;
		}
	}
	for (const std::string& Value : Options.Packages)
	{
		Durin::FAssetPath Path;
		if (!Durin::FAssetPath::TryCreate(Value, Path, &Error))
		{
			std::cerr << "Error: invalid --package value '" << Value << "': " << Error << '\n';
			return 2;
		}
		Selection.Packages.push_back(std::move(Path));
	}
	for (const std::string& Mount : Selection.Mounts)
	{
		const bool bFound = std::ranges::any_of(Records, [&](const auto& Record) {
			const std::string_view Path = Record.PackagePath.GetView();
			return Path.starts_with(Mount) && Path.size() > Mount.size() && Path[Mount.size()] == '/';
		});
		if (!bFound)
		{
			std::cerr << "Error: --mount selector '" << Mount << "' matched no discovered package.\n";
			return 1;
		}
	}
	for (const Durin::FAssetPath& Package : Selection.Packages)
	{
		if (std::ranges::find(Records, Package, &Durin::Asset::FAssetPackageCompatibilityRecord::PackagePath) != Records.end()) continue;
		std::cerr << "Error: --package selector '" << Package.ToString() << "' matched no discovered package.\n";
		return 1;
	}
	const auto Plan = Durin::Asset::PlanAssetPackageMigrations(
		Records, Registry, Selection, [] { return GCancelled.load(std::memory_order_relaxed); });
	if (Plan.Status == Durin::Asset::EAssetMigrationPlanStatus::Cancelled) return 130;
	if (Options.bApply)
	{
		auto ApplyResult = Durin::Asset::ApplyAssetPackageMigrations(
			Plan, Catalog, {}, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (ApplyResult.Status == Durin::Asset::EAssetMigrationApplyStatus::Cancelled) return 130;
		std::cout << Durin::Asset::SerializeAssetMigrationApplyReportV1(ApplyResult) << '\n';
		return 0;
	}
	std::cout << Durin::Asset::SerializeAssetMigrationPlanReportV1(Plan) << '\n';
	return 0;
}

#include "AssetTools.h"
#include "ImportRecord.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EngineAssetServices.h"
#include "Engine/Level.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Logging/Logger.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include <csignal>

namespace
{
	std::atomic_bool GCancelled = false;

	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	struct FOptions
	{
		std::string Project;
		bool bCanonicalResave = false;
		bool bCi = false;
		bool bProjectScope = false;
		bool bHuman = false;
		bool bApply = false;
		std::vector<std::string> Mounts;
		std::vector<std::string> Folders;
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
			else if (Argument == "--operation=canonical-resave") OutOptions.bCanonicalResave = true;
			else if (Argument == "--ci") OutOptions.bCi = true;
			else if (Argument == "--project-scope") OutOptions.bProjectScope = true;
			else if (Argument == "--apply") OutOptions.bApply = true;
			else if (Argument.starts_with("--mount=")) OutOptions.Mounts.emplace_back(Argument.substr(std::string_view("--mount=").size()));
			else if (Argument.starts_with("--folder=")) OutOptions.Folders.emplace_back(Argument.substr(std::string_view("--folder=").size()));
			else if (Argument.starts_with("--package=")) OutOptions.Packages.emplace_back(Argument.substr(std::string_view("--package=").size()));
			else if (Argument == "--format=human") OutOptions.bHuman = true;
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
		if (OutOptions.bApply && !OutOptions.bCanonicalResave)
		{
			OutError = "--apply requires --operation=canonical-resave.";
			return false;
		}
		if (OutOptions.bCi && !OutOptions.bCanonicalResave)
		{
			OutError = "--ci requires --operation=canonical-resave.";
			return false;
		}
		if (OutOptions.bCi && OutOptions.bApply)
		{
			OutError = "--ci is read-only and cannot be combined with --apply.";
			return false;
		}
		if (OutOptions.bCanonicalResave && OutOptions.Mounts.empty()
			&& OutOptions.Folders.empty() && OutOptions.Packages.empty()
			&& !OutOptions.bProjectScope)
		{
			OutError = "Canonical resave requires --package, --folder, --mount, or explicit --project-scope.";
			return false;
		}
		if (OutOptions.bCanonicalResave && OutOptions.bProjectScope
			&& (!OutOptions.Mounts.empty() || !OutOptions.Folders.empty()
				|| !OutOptions.Packages.empty()))
		{
			OutError = "--project-scope cannot be combined with narrower selectors.";
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
	if (!Durin::InitializeCurrentProject(
			{.RequestedProjectFile = Options.Project}, &Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	// Project initialization configures logging. From this point stdout is the
	// machine-readable report channel consumed by DurinDevTool.
	Durin::LoggerInit();
	Durin::FLogger::Get().SetConsoleLogLevel(Durin::ELogLevel::Fatal);
	if (!Durin::PathUtilities::InitDefaultMountPoints(&Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::DObjectInit();
	Durin::InitializeEngineAssetServices();
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
	(void)Durin::Asset::DImportRecord::StaticClass(); // AssetImport packages are part of the authored corpus.
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
	if (Options.bCanonicalResave && Options.bApply)
	{
		const Durin::Asset::FAssetCatalogRefreshResult Refresh =
			Durin::Asset::RefreshAssetCatalog(
				Durin::Asset::EAssetRegistryScanMode::FullValidation);
		if (!Refresh || !Refresh.bPublished)
		{
			std::cerr << "Error: canonical-resave requires a complete published asset catalog.\n";
			return 1;
		}
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
	if (!Options.bCanonicalResave)
	{
		std::cout << Durin::Asset::SerializeAssetCompatibilityReportV1(Records) << '\n';
		return 0;
	}

	if (Options.bCanonicalResave)
	{
		Durin::Asset::FAssetCanonicalResaveSelection Selection{
			.Mounts = std::move(Options.Mounts),
			.Folders = std::move(Options.Folders)};
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
			if (Mount.size() < 2 || Mount.front() != '/' || Mount.back() == '/'
				|| Mount.find("//") != std::string::npos || Mount.find('\\') != std::string::npos)
			{
				std::cerr << "Error: invalid --mount value '" << Mount << "'.\n";
				return 2;
			}
			if (!std::ranges::any_of(Records, [&](const auto& Record) {
				const std::string_view Path = Record.PackagePath.GetView();
				return Path.starts_with(Mount) && Path.size() > Mount.size() && Path[Mount.size()] == '/';
			}))
			{
				std::cerr << "Error: --mount selector '" << Mount << "' matched no discovered package.\n";
				return 1;
			}
		}
		for (const std::string& Folder : Selection.Folders)
		{
			if (Folder.size() < 2 || Folder.front() != '/' || Folder.back() == '/'
				|| Folder.find("//") != std::string::npos || Folder.find('\\') != std::string::npos)
			{
				std::cerr << "Error: invalid --folder value '" << Folder << "'.\n";
				return 2;
			}
			if (!std::ranges::any_of(Records, [&](const auto& Record) {
				const std::string_view Path = Record.PackagePath.GetView();
				return Path.starts_with(Folder) && Path.size() > Folder.size() && Path[Folder.size()] == '/';
			}))
			{
				std::cerr << "Error: --folder selector '" << Folder << "' matched no discovered package.\n";
				return 1;
			}
		}
		for (const Durin::FAssetPath& Package : Selection.Packages)
			if (std::ranges::find(Records, Package,
				&Durin::Asset::FAssetPackageCompatibilityRecord::PackagePath) == Records.end())
			{
				std::cerr << "Error: --package selector '" << Package.ToString()
					<< "' matched no discovered package.\n";
				return 1;
			}
		Selection.bWholeProject = Options.bProjectScope;
		const auto Plan = Durin::Asset::PlanAssetCanonicalResaves(
			Records, Selection, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (Plan.Status == Durin::Asset::EAssetCanonicalResavePlanStatus::Cancelled) return 130;
		if (Options.bApply)
		{
			auto Applied = Durin::Asset::ApplyAssetCanonicalResaves(
				Plan, Catalog, {}, [] { return GCancelled.load(std::memory_order_relaxed); });
			if (Options.bHuman)
				std::cout << "canonical-resave apply: " << Applied.ChangedPaths.size()
					<< " package(s) resaved; " << Applied.Diagnostic << '\n';
			else std::cout << Durin::Asset::SerializeAssetCanonicalResaveApplyReport(Applied) << '\n';
			if (Applied.Status == Durin::Asset::EAssetCanonicalResaveApplyStatus::Cancelled) return 130;
			return Applied.Status == Durin::Asset::EAssetCanonicalResaveApplyStatus::Succeeded ? 0 : 1;
		}
		if (Options.bHuman)
		{
			const auto Ready = std::ranges::count(Plan.Packages,
				Durin::Asset::EAssetCanonicalResavePackageStatus::Ready,
				&Durin::Asset::FAssetCanonicalResavePackagePlan::Status);
			const auto Blocked = std::ranges::count(Plan.Packages,
				Durin::Asset::EAssetCanonicalResavePackageStatus::Blocked,
				&Durin::Asset::FAssetCanonicalResavePackagePlan::Status);
			std::cout << "canonical-resave plan: " << Ready << " ready, "
				<< Blocked << " blocked, " << Plan.Packages.size() << " selected\n";
		}
		else std::cout << Durin::Asset::SerializeAssetCanonicalResavePlanReport(Plan) << '\n';
		if (Options.bCi && std::ranges::any_of(Plan.Packages, [](const auto& Package) {
			return Package.Status != Durin::Asset::EAssetCanonicalResavePackageStatus::Skipped;
		})) return 3;
		return 0;
	}

	return 0;
}

#include "AssetTools.h"
#include "ImportRecord.h"

#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageInspection.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "EngineAssetServices.h"
#include "Engine/Level.h"
#include "ImportService.h"
#include "HAL/PlatformMisc.h"
#include "Logging/Logger.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Threading/Task.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

#include <chrono>
#include <csignal>
#include <thread>

namespace
{
	std::atomic_bool GCancelled = false;
	constexpr auto CanonicalResaveRecoveryTimeout = std::chrono::seconds(60);

	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	auto ValidateCanonicalResaveAssetReadiness(Durin::DObject* Asset)
		-> Durin::Asset::FAssetResult
	{
		if (!Asset)
			return {Durin::Asset::EAssetError::InvalidObjectGraph,
				"Loaded package has no main asset."};
		auto NotReady = [&](std::string_view Domain) {
			return Durin::Asset::FAssetResult{
				Durin::Asset::EAssetError::StaleData,
				std::format("{} post-load recovery did not publish domain-ready data.", Domain)};
		};
		if (const auto* Mesh = Durin::Cast<Durin::DStaticMesh>(Asset);
			Mesh && !Mesh->GetRenderData()) return NotReady("StaticMesh");
		if (const auto* Mesh = Durin::Cast<Durin::DSkeletalMesh>(Asset);
			Mesh && !Mesh->GetRenderData()) return NotReady("SkeletalMesh");
		if (const auto* Texture = Durin::Cast<Durin::DTexture2D>(Asset);
			Texture && (!Texture->GetPlatformData()
				|| Texture->GetBuildStatus() != Durin::ETextureBuildStatus::Ready))
			return NotReady("Texture2D");
		if (const auto* Texture = Durin::Cast<Durin::DTextureCube>(Asset);
			Texture && (!Texture->GetPlatformData()
				|| Texture->GetBuildStatus() != Durin::ETextureBuildStatus::Ready))
			return NotReady("TextureCube");
		if (const auto* Texture = Durin::Cast<Durin::DVolumeTexture>(Asset);
			Texture && (!Texture->GetPlatformData()
				|| Texture->GetBuildStatus() != Durin::ETextureBuildStatus::Ready))
			return NotReady("VolumeTexture");
		if (const auto* Heightmap = Durin::Cast<Durin::DTerrainHeightmap>(Asset);
			Heightmap && (!Heightmap->GetPayload()
				|| Heightmap->GetStatus() != Durin::ETerrainHeightmapStatus::Ready))
			return NotReady("TerrainHeightmap");
		return {};
	}

	auto PrepareCanonicalResaveAsset(
		const Durin::FAssetPath& Path, Durin::DObject* Asset)
		-> Durin::Asset::FAssetResult
	{
		auto& Service = Durin::Asset::GetImportService();
		const auto Deadline = std::chrono::steady_clock::now()
			+ CanonicalResaveRecoveryTimeout;
		while (Service.HasActiveImportClaim(Path.ToString()))
		{
			if (GCancelled.load(std::memory_order_relaxed))
				return {Durin::Asset::EAssetError::ShuttingDown,
					"Canonical resave was cancelled while waiting for post-load recovery."};
			if (std::chrono::steady_clock::now() >= Deadline)
				return {Durin::Asset::EAssetError::StaleData,
					std::format("Timed out waiting for post-load recovery of {}.",
						Path.ToString())};
			(void)Service.PumpImportOperations();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		(void)Service.PumpImportOperations();
		return ValidateCanonicalResaveAssetReadiness(Asset);
	}

	struct FOptions
	{
		std::string Project;
		bool bCanonicalResave = false;
		bool bStorageQualificationInventory = false;
		bool bCi = false;
		bool bProjectScope = false;
		bool bHuman = false;
		bool bApply = false;
		bool bTargetSpecified = false;
		Durin::Asset::EAssetPackageWriterSelection TargetWriterSelection =
			Durin::Asset::EAssetPackageWriterSelection::DastV5;
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
			else if (Argument == "--operation=storage-qualification-inventory")
				OutOptions.bStorageQualificationInventory = true;
			else if (Argument == "--ci") OutOptions.bCi = true;
			else if (Argument == "--project-scope") OutOptions.bProjectScope = true;
			else if (Argument == "--apply") OutOptions.bApply = true;
			else if (Argument == "--target=v5")
			{
				OutOptions.bTargetSpecified = true;
				OutOptions.TargetWriterSelection =
					Durin::Asset::EAssetPackageWriterSelection::DastV5;
			}
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
		if (OutOptions.bTargetSpecified && !OutOptions.bCanonicalResave)
		{
			OutError = "--target requires --operation=canonical-resave.";
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
		if (OutOptions.bCanonicalResave && OutOptions.bStorageQualificationInventory)
		{
			OutError = "Only one --operation may be selected.";
			return false;
		}
		return true;
	}

	auto JsonEscape(std::string_view Value) -> std::string
	{
		std::string Escaped;
		Escaped.reserve(Value.size());
		for (const unsigned char Character : Value)
		{
			switch (Character)
			{
			case '\"': Escaped += "\\\""; break;
			case '\\': Escaped += "\\\\"; break;
			case '\b': Escaped += "\\b"; break;
			case '\f': Escaped += "\\f"; break;
			case '\n': Escaped += "\\n"; break;
			case '\r': Escaped += "\\r"; break;
			case '\t': Escaped += "\\t"; break;
			default:
				if (Character < 0x20) Escaped += std::format("\\u{:04x}", Character);
				else Escaped.push_back(static_cast<char>(Character));
			}
		}
		return Escaped;
	}

	struct FQualificationDescriptor
	{
		Durin::Asset::FEditorBulkDataStorageDescriptor Descriptor;
		std::filesystem::path CompanionPath;
		bool bReachable = true;
		std::string Diagnostic;
		Durin::FSharedByteBuffer ExternalBytes;
		uint64 ExactDuplicateGroup = 0;
	};

	auto SerializeStorageQualificationInventory(
		std::span<const Durin::Asset::FAssetPackageCompatibilityProbeInput> Inputs)
		-> std::string
	{
		using namespace Durin;
		using namespace Durin::Asset;
		struct FPackage
		{
			const FAssetPackageCompatibilityProbeInput* Input = nullptr;
			FAssetPackageInspection Inspection;
			FAssetResult Result;
			std::vector<uint64> InspectionNanoseconds;
			std::vector<FQualificationDescriptor> Descriptors;
			std::vector<std::filesystem::path> Orphans;
			std::string DescriptorDiagnostic;
		};

		std::vector<FPackage> Packages;
		Packages.reserve(Inputs.size());
		for (const FAssetPackageCompatibilityProbeInput& Input : Inputs)
		{
			FPackage Package;
			Package.Input = &Input;
			for (size_t Repeat = 0; Repeat < 5; ++Repeat)
			{
				FAssetPackageInspection Candidate;
				const auto Start = std::chrono::steady_clock::now();
				Package.Result = InspectAssetPackage(Input.PhysicalPath, Candidate);
				const auto End = std::chrono::steady_clock::now();
				Package.InspectionNanoseconds.push_back(static_cast<uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(End - Start).count()));
				if (!Package.Result) break;
				Package.Inspection = std::move(Candidate);
			}
			if (Package.Result)
			{
				std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
				if (!InspectEditorBulkDataStorageDescriptors(
						Package.Inspection, Descriptors, &Package.DescriptorDiagnostic))
				{
					Package.Result = {EAssetError::CorruptFile, Package.DescriptorDiagnostic};
				}
				else
				{
					for (const FEditorBulkDataStorageDescriptor& Descriptor : Descriptors)
					{
						FQualificationDescriptor Item{.Descriptor = Descriptor};
						if (Descriptor.StorageKind == EEditorBulkDataStorageKind::External)
						{
							if (!ResolveEditorBulkDataCompanionPath(
									Input.PhysicalPath, Item.CompanionPath, &Item.Diagnostic)
								|| !ReadEditorBulkDataStoragePayload(
									Item.CompanionPath, Descriptor, Item.ExternalBytes,
									&Item.Diagnostic))
								Item.bReachable = false;
						}
						Package.Descriptors.push_back(std::move(Item));
					}
					std::string OrphanError;
					if (!InspectOrphanedEditorBulkDataCompanionPaths(
							Input.PhysicalPath, Package.Inspection, Package.Orphans, &OrphanError))
						Package.DescriptorDiagnostic = OrphanError;
				}
			}
			Packages.push_back(std::move(Package));
		}

		uint64 NextDuplicateGroup = 1;
		std::vector<FQualificationDescriptor*> External;
		for (FPackage& Package : Packages)
			for (FQualificationDescriptor& Descriptor : Package.Descriptors)
				if (Descriptor.bReachable && !Descriptor.ExternalBytes.IsEmpty())
					External.push_back(&Descriptor);
		for (size_t LeftIndex = 0; LeftIndex < External.size(); ++LeftIndex)
		{
			FQualificationDescriptor& Left = *External[LeftIndex];
			for (size_t RightIndex = LeftIndex + 1; RightIndex < External.size(); ++RightIndex)
			{
				FQualificationDescriptor& Right = *External[RightIndex];
				if (Left.Descriptor.ContentHash != Right.Descriptor.ContentHash
					|| Left.ExternalBytes.GetSize() != Right.ExternalBytes.GetSize()
					|| !std::ranges::equal(Left.ExternalBytes.GetBytes(), Right.ExternalBytes.GetBytes()))
					continue;
				if (Left.ExactDuplicateGroup == 0) Left.ExactDuplicateGroup = NextDuplicateGroup++;
				Right.ExactDuplicateGroup = Left.ExactDuplicateGroup;
			}
		}

		std::string Json = "{\"schemaVersion\":2,\"inspectionRepeatCount\":5,\"packages\":[";
		for (size_t PackageIndex = 0; PackageIndex < Packages.size(); ++PackageIndex)
		{
			if (PackageIndex != 0) Json += ',';
			const FPackage& Package = Packages[PackageIndex];
			Json += std::format(
				"{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"fileSize\":{},"
				"\"inspection\":\"{}\",\"diagnostic\":\"{}\",\"formatVersion\":{},\"fileBytesRead\":{},"
				"\"inspectionNanoseconds\":[",
				JsonEscape(Package.Input->PackagePath.GetView()), JsonEscape(Package.Input->PhysicalPath),
				Package.Input->ExpectedFileSize, Package.Result ? "Ready" : "Failed",
				JsonEscape(Package.Result.Message), Package.Inspection.Header.FormatVersion,
				Package.Input->ExpectedFileSize);
			for (size_t Index = 0; Index < Package.InspectionNanoseconds.size(); ++Index)
			{
				if (Index != 0) Json += ',';
				Json += std::to_string(Package.InspectionNanoseconds[Index]);
			}
			Json += "],\"descriptors\":[";
			for (size_t DescriptorIndex = 0; DescriptorIndex < Package.Descriptors.size(); ++DescriptorIndex)
			{
				if (DescriptorIndex != 0) Json += ',';
				const FQualificationDescriptor& Item = Package.Descriptors[DescriptorIndex];
				const auto& Descriptor = Item.Descriptor;
				Json += std::format(
					"{{\"payloadId\":\"{}\",\"logicalBytes\":{},\"storedBytes\":{},"
					"\"contentHash\":\"{}\",\"containerHash\":\"{}\",\"storage\":\"{}\","
					"\"companionPath\":\"{}\",\"reachable\":{},\"diagnostic\":\"{}\","
					"\"exactDuplicateGroup\":{}}}",
					Descriptor.PayloadId.ToString(), Descriptor.LogicalByteCount,
					Descriptor.StoredByteCount, Descriptor.ContentHash.ToString(),
					Descriptor.ContainerHash.ToString(),
					Descriptor.StorageKind == EEditorBulkDataStorageKind::Inline ? "Inline" : "External",
					JsonEscape(Item.CompanionPath.generic_string()), Item.bReachable ? "true" : "false",
					JsonEscape(Item.Diagnostic), Item.ExactDuplicateGroup);
			}
			Json += "],\"orphanCompanions\":[";
			for (size_t OrphanIndex = 0; OrphanIndex < Package.Orphans.size(); ++OrphanIndex)
			{
				if (OrphanIndex != 0) Json += ',';
				Json += std::format("\"{}\"", JsonEscape(Package.Orphans[OrphanIndex].generic_string()));
			}
			Json += std::format("],\"descriptorDiagnostic\":\"{}\"}}",
				JsonEscape(Package.DescriptorDiagnostic));
		}
		Json += "]}";
		return Json;
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
	Durin::FPlatformMisc::EnableUserBinaryDirectoriesSearch();
	Durin::FPlatformMisc::AddRuntimeBinaryDirectory(
		Durin::FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());
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
	if (Options.bCanonicalResave && Options.bApply)
	{
		if (!Durin::InitializeTaskScheduler(2)
			|| !Durin::InitializeGameThreadDeferredExecutor())
		{
			std::cerr << "Error: canonical-resave apply could not initialize task services.\n";
			return 1;
		}
		Durin::FModuleManager::Get().LoadModuleChecked("GeometryBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForge");
	}
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
	if (Options.bStorageQualificationInventory)
	{
		std::cout << SerializeStorageQualificationInventory(Snapshot.Packages) << '\n';
		return 0;
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
		Selection.bAllowPlainResave = true;
		Selection.TargetWriterSelection = Options.TargetWriterSelection;
		const auto Plan = Durin::Asset::PlanAssetCanonicalResaves(
			Records, Selection, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (Plan.Status == Durin::Asset::EAssetCanonicalResavePlanStatus::Cancelled) return 130;
		if (Options.bApply)
		{
			auto Applied = Durin::Asset::ApplyAssetCanonicalResaves(
				Plan, Catalog,
				{.PrepareLoadedAsset = PrepareCanonicalResaveAsset},
				[] { return GCancelled.load(std::memory_order_relaxed); });
			if (Options.bHuman)
				std::cout << "canonical-resave apply: " << Applied.ChangedPaths.size()
					<< " package(s) resaved; " << Applied.Diagnostic << '\n';
			else std::cout << Durin::Asset::SerializeAssetCanonicalResaveApplyReport(Applied) << '\n';
			std::cout.flush();
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

#include "AssetTools.h"
#include "AssetForge/Persistence/ImportRecord.h"
#include "AssetAuthoringReadiness.h"

#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageInspection.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EngineAssetServices.h"
#include "Engine/Level.h"
#include "AssetForge/ImportService.h"
#include "HAL/PlatformMisc.h"
#include "Logging/Logger.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"

#include <chrono>
#include <csignal>
#include <thread>

namespace
{
	std::atomic_bool GCancelled = false;
	constexpr auto CanonicalResaveRecoveryTimeout = std::chrono::seconds(60);

	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	auto PrepareCanonicalResaveAsset(
		const Durin::FAssetPath& Path, Durin::DObject* Asset)
		-> Durin::Asset::FAssetResult
	{
		auto& Service = Durin::AssetForge::GetImportService();
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
		return Durin::ValidateAssetAuthoringReadiness(Asset);
	}

	enum class EOperation : uint8
	{
		Audit,
		CanonicalResave,
		StorageQualificationInventory
	};

	enum class EOutputFormat : uint8 { Json, Human };

	enum class EOption : uint16
	{
		Project = 1 << 0,
		Operation = 1 << 1,
		Format = 1 << 2,
		Ci = 1 << 3,
		ProjectScope = 1 << 4,
		Apply = 1 << 5,
		Mount = 1 << 6,
		Folder = 1 << 7,
		Package = 1 << 8
	};

	constexpr auto OptionBit(EOption Option) -> uint16
	{
		return static_cast<uint16>(Option);
	}

	auto OperationName(EOperation Operation) -> std::string_view
	{
		switch (Operation)
		{
		case EOperation::Audit: return "audit";
		case EOperation::CanonicalResave: return "canonical-resave";
		case EOperation::StorageQualificationInventory:
			return "storage-qualification-inventory";
		}
		return "audit";
	}

	auto OptionName(EOption Option) -> std::string_view
	{
		switch (Option)
		{
		case EOption::Project: return "--project";
		case EOption::Operation: return "--operation";
		case EOption::Format: return "--format";
		case EOption::Ci: return "--ci";
		case EOption::ProjectScope: return "--project-scope";
		case EOption::Apply: return "--apply";
		case EOption::Mount: return "--mount";
		case EOption::Folder: return "--folder";
		case EOption::Package: return "--package";
		}
		return "option";
	}

	struct FOptions
	{
		std::string Project;
		EOperation Operation = EOperation::Audit;
		EOutputFormat Format = EOutputFormat::Json;
		bool bCi = false;
		bool bProjectScope = false;
		bool bApply = false;
		std::vector<std::string> Mounts;
		std::vector<std::string> Folders;
		std::vector<std::string> Packages;
		uint16 SpecifiedOptions = 0;
	};

	auto MarkOptionOnce(FOptions& Options, EOption Option, std::string& OutError) -> bool
	{
		const uint16 Bit = OptionBit(Option);
		if ((Options.SpecifiedOptions & Bit) != 0)
		{
			OutError = std::format("{} may be specified only once.", OptionName(Option));
			return false;
		}
		Options.SpecifiedOptions |= Bit;
		return true;
	}

	auto ValidateOptions(const FOptions& Options, std::string& OutError) -> bool
	{
		if (Options.Project.empty())
		{
			OutError = "--project=<path-to-project.dproject> is required.";
			return false;
		}

		constexpr uint16 Common = OptionBit(EOption::Project)
			| OptionBit(EOption::Operation) | OptionBit(EOption::Format);
		constexpr uint16 Canonical = Common | OptionBit(EOption::Ci)
			| OptionBit(EOption::ProjectScope) | OptionBit(EOption::Apply)
			| OptionBit(EOption::Mount) | OptionBit(EOption::Folder)
			| OptionBit(EOption::Package);
		const uint16 Allowed = Options.Operation == EOperation::CanonicalResave
			? Canonical : Common;
		const uint16 Unexpected = Options.SpecifiedOptions & ~Allowed;
		constexpr EOption OrderedOptions[] = {
			EOption::Ci, EOption::ProjectScope, EOption::Apply, EOption::Mount,
			EOption::Folder, EOption::Package};
		for (const EOption Option : OrderedOptions)
			if ((Unexpected & OptionBit(Option)) != 0)
			{
				OutError = std::format("{} is not valid for operation {}.",
					OptionName(Option), OperationName(Options.Operation));
				return false;
			}
		if (Options.Operation != EOperation::CanonicalResave
			&& Options.Format == EOutputFormat::Human)
		{
			OutError = std::format(
				"--format=human is not supported by operation {}.",
				OperationName(Options.Operation));
			return false;
		}
		if (Options.Operation != EOperation::CanonicalResave) return true;
		if (Options.bCi && Options.bApply)
		{
			OutError = "--ci is read-only and cannot be combined with --apply.";
			return false;
		}
		if (Options.Mounts.empty() && Options.Folders.empty()
			&& Options.Packages.empty() && !Options.bProjectScope)
		{
			OutError = "Canonical resave requires --package, --folder, --mount, or explicit --project-scope.";
			return false;
		}
		if (Options.bProjectScope && (!Options.Mounts.empty()
			|| !Options.Folders.empty() || !Options.Packages.empty()))
		{
			OutError = "--project-scope cannot be combined with narrower selectors.";
			return false;
		}
		return true;
	}

	auto ParseOptions(int ArgC, char** ArgV, FOptions& OutOptions, std::string& OutError) -> bool
	{
		for (int Index = 1; Index < ArgC; ++Index)
		{
			const std::string_view Argument = ArgV[Index];
			if (Argument.starts_with("--project="))
			{
				if (!MarkOptionOnce(OutOptions, EOption::Project, OutError)) return false;
				OutOptions.Project = Argument.substr(std::string_view("--project=").size());
			}
			else if (Argument == "--operation=audit"
				|| Argument == "--operation=canonical-resave"
				|| Argument == "--operation=storage-qualification-inventory")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Operation, OutError)) return false;
				if (Argument == "--operation=canonical-resave")
					OutOptions.Operation = EOperation::CanonicalResave;
				else if (Argument == "--operation=storage-qualification-inventory")
					OutOptions.Operation = EOperation::StorageQualificationInventory;
			}
			else if (Argument == "--ci")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Ci, OutError)) return false;
				OutOptions.bCi = true;
			}
			else if (Argument == "--project-scope")
			{
				if (!MarkOptionOnce(OutOptions, EOption::ProjectScope, OutError)) return false;
				OutOptions.bProjectScope = true;
			}
			else if (Argument == "--apply")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Apply, OutError)) return false;
				OutOptions.bApply = true;
			}
			else if (Argument.starts_with("--mount="))
			{
				OutOptions.SpecifiedOptions |= OptionBit(EOption::Mount);
				OutOptions.Mounts.emplace_back(Argument.substr(std::string_view("--mount=").size()));
			}
			else if (Argument.starts_with("--folder="))
			{
				OutOptions.SpecifiedOptions |= OptionBit(EOption::Folder);
				OutOptions.Folders.emplace_back(Argument.substr(std::string_view("--folder=").size()));
			}
			else if (Argument.starts_with("--package="))
			{
				OutOptions.SpecifiedOptions |= OptionBit(EOption::Package);
				OutOptions.Packages.emplace_back(Argument.substr(std::string_view("--package=").size()));
			}
			else if (Argument == "--format=human" || Argument == "--format=json")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Format, OutError)) return false;
				OutOptions.Format = Argument == "--format=human"
					? EOutputFormat::Human : EOutputFormat::Json;
			}
			else
			{
				OutError = std::format("Unknown argument: {}", Argument);
				return false;
			}
		}
		return ValidateOptions(OutOptions, OutError);
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

	auto IsValidVirtualPrefix(std::string_view Value) -> bool
	{
		return Value.size() >= 2 && Value.front() == '/' && Value.back() != '/'
			&& Value.find("//") == std::string_view::npos
			&& Value.find('\\') == std::string_view::npos;
	}

	auto MatchesVirtualPrefix(
		const Durin::Asset::FAssetPackageCompatibilityRecord& Record,
		std::string_view Prefix) -> bool
	{
		const std::string_view Path = Record.PackagePath.GetView();
		return Path.starts_with(Prefix) && Path.size() > Prefix.size()
			&& Path[Prefix.size()] == '/';
	}

	auto MakeCanonicalResaveSelection(
		const FOptions& Options,
		std::span<const Durin::Asset::FAssetPackageCompatibilityRecord> Records,
		Durin::Asset::FAssetCanonicalResaveSelection& OutSelection,
		std::string& OutError) -> int
	{
		OutSelection = {
			.Mounts = Options.Mounts,
			.Folders = Options.Folders,
			.bWholeProject = Options.bProjectScope,
			.bAllowPlainResave = true};
		for (const std::string& Value : Options.Packages)
		{
			Durin::FAssetPath Path;
			if (!Durin::FAssetPath::TryCreate(Value, Path, &OutError))
			{
				OutError = std::format(
					"invalid --package value '{}': {}", Value, OutError);
				return 2;
			}
			OutSelection.Packages.push_back(std::move(Path));
		}

		auto ValidatePrefixes = [&](std::span<const std::string> Values,
			std::string_view OptionName) -> int {
			for (const std::string& Value : Values)
			{
				if (!IsValidVirtualPrefix(Value))
				{
					OutError = std::format("invalid {} value '{}'.", OptionName, Value);
					return 2;
				}
				if (std::ranges::none_of(Records, [&](const auto& Record) {
					return MatchesVirtualPrefix(Record, Value);
				}))
				{
					OutError = std::format(
						"{} selector '{}' matched no discovered package.", OptionName, Value);
					return 1;
				}
			}
			return 0;
		};
		if (const int Result = ValidatePrefixes(OutSelection.Mounts, "--mount"))
			return Result;
		if (const int Result = ValidatePrefixes(OutSelection.Folders, "--folder"))
			return Result;
		for (const Durin::FAssetPath& Package : OutSelection.Packages)
			if (std::ranges::find(Records, Package,
				&Durin::Asset::FAssetPackageCompatibilityRecord::PackagePath) == Records.end())
			{
				OutError = std::format("--package selector '{}' matched no discovered package.",
					Package.ToString());
				return 1;
			}
		return 0;
	}

	auto RunCanonicalResave(
		const FOptions& Options,
		std::span<const Durin::Asset::FAssetPackageCompatibilityRecord> Records,
		const Durin::Asset::FReflectionCompatibilityCatalog& Catalog) -> int
	{
		std::string Error;
		Durin::Asset::FAssetCanonicalResaveSelection Selection;
		if (const int Result = MakeCanonicalResaveSelection(
				Options, Records, Selection, Error))
		{
			std::cerr << "Error: " << Error << '\n';
			return Result;
		}
		const auto Plan = Durin::Asset::PlanAssetCanonicalResaves(
			Records, Selection, [] { return GCancelled.load(std::memory_order_relaxed); });
		if (Plan.Status == Durin::Asset::EAssetCanonicalResavePlanStatus::Cancelled)
			return 130;
		if (Options.bApply)
		{
			auto Applied = Durin::Asset::ApplyAssetCanonicalResaves(
				Plan, Catalog,
				{.PrepareLoadedAsset = PrepareCanonicalResaveAsset},
				[] { return GCancelled.load(std::memory_order_relaxed); });
			if (Options.Format == EOutputFormat::Human)
				std::cout << "canonical-resave apply: " << Applied.ChangedPaths.size()
					<< " package(s) resaved; " << Applied.Diagnostic << '\n';
			else
				std::cout << Durin::Asset::SerializeAssetCanonicalResaveApplyReport(Applied)
					<< '\n';
			std::cout.flush();
			if (Applied.Status == Durin::Asset::EAssetCanonicalResaveApplyStatus::Cancelled)
				return 130;
			return Applied.Status == Durin::Asset::EAssetCanonicalResaveApplyStatus::Succeeded
				? 0 : 1;
		}
		if (Options.Format == EOutputFormat::Human)
		{
			const auto Ready = std::ranges::count(Plan.Packages,
				Durin::Asset::EAssetCanonicalResavePackageStatus::Ready,
				&Durin::Asset::FAssetCanonicalResavePackagePlan::Status);
			const auto Blocked = std::ranges::count(Plan.Packages,
				Durin::Asset::EAssetCanonicalResavePackageStatus::Blocked,
				&Durin::Asset::FAssetCanonicalResavePackagePlan::Status);
			const auto Skipped = std::ranges::count(Plan.Packages,
				Durin::Asset::EAssetCanonicalResavePackageStatus::Skipped,
				&Durin::Asset::FAssetCanonicalResavePackagePlan::Status);
			std::cout << "canonical-resave plan: " << Ready << " ready, "
				<< Blocked << " blocked, " << Skipped << " skipped, "
				<< Plan.Packages.size() << " selected\n";
			for (const auto& Package : Plan.Packages)
				if (Package.Status == Durin::Asset::EAssetCanonicalResavePackageStatus::Blocked)
				{
					std::cout << "  " << Package.PackagePath.ToString() << '\n';
					for (const std::string& Diagnostic : Package.Diagnostics)
						std::cout << "    " << Diagnostic << '\n';
				}
		}
		else
			std::cout << Durin::Asset::SerializeAssetCanonicalResavePlanReport(Plan) << '\n';
		if (Options.bCi && std::ranges::any_of(Plan.Packages, [](const auto& Package) {
			return Package.Status != Durin::Asset::EAssetCanonicalResavePackageStatus::Skipped;
		})) return 3;
		return 0;
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
	struct FScopedLoggerShutdown final
	{
		~FScopedLoggerShutdown() { Durin::LoggerShutdown(); }
	} ScopedLoggerShutdown;
	Durin::FLogger::Get().SetConsoleLogLevel(Durin::ELogLevel::Fatal);
	if (!Durin::PathUtilities::InitDefaultMountPoints(&Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::DObjectInit();
	Durin::InitializeEngineAssetServices();
	if (Options.Operation == EOperation::CanonicalResave && Options.bApply)
	{
		if (!Durin::InitializeTaskScheduler(2)
			|| !Durin::InitializeGameThreadDeferredExecutor())
		{
			std::cerr << "Error: canonical-resave apply could not initialize task services.\n";
			return 1;
		}
		Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("SkeletalBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("TerrainBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	}
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
	(void)Durin::AssetForge::DImportRecord::StaticClass(); // AssetImport packages are part of the authored corpus.
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
	if (Options.Operation == EOperation::StorageQualificationInventory)
	{
		std::cout << SerializeStorageQualificationInventory(Snapshot.Packages) << '\n';
		return 0;
	}
	if (Options.Operation == EOperation::CanonicalResave && Options.bApply)
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
	if (Options.Operation == EOperation::Audit)
	{
		std::cout << Durin::Asset::SerializeAssetCompatibilityReportV1(Records) << '\n';
		return 0;
	}

	return RunCanonicalResave(Options, Records, Catalog);
}

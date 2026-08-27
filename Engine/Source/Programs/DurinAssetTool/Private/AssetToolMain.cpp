#include "AssetTools.h"
#include "AssetSaveReadiness.h"

#include "Asset/EditorBulkDataStorage.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageInspection.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EngineAssetServices.h"
#include "Engine/Level.h"
#include "HAL/PlatformMisc.h"
#include "Logging/Logger.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"

#include <chrono>
#include <csignal>

namespace
{
	std::atomic_bool GCancelled = false;
	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	auto PrepareCanonicalResaveAsset(
		const Durin::FAssetPath& Path, Durin::DObject* Asset)
		-> Durin::Asset::FAssetResult
	{
		if (GCancelled.load(std::memory_order_relaxed))
			return {Durin::Asset::EAssetError::ShuttingDown,
				"Canonical resave was cancelled before asset compilation completed."};
		(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Asset);
		return Durin::ValidateAssetSaveReadiness(Asset);
	}

	enum class EOperation : uint8
	{
		Check,
		Resave,
		StorageInventory
	};

	enum class EOutputFormat : uint8 { Json, Human };

	enum class EOption : uint16
	{
		Project = 1 << 0,
		Json = 1 << 1,
		All = 1 << 2,
		Apply = 1 << 3,
		Scope = 1 << 4
	};

	constexpr auto OptionBit(EOption Option) -> uint16
	{
		return static_cast<uint16>(Option);
	}

	auto OperationName(EOperation Operation) -> std::string_view
	{
		switch (Operation)
		{
		case EOperation::Check: return "check";
		case EOperation::Resave: return "resave";
		case EOperation::StorageInventory: return "storage-inventory";
		}
		return "check";
	}

	auto OptionName(EOption Option) -> std::string_view
	{
		switch (Option)
		{
		case EOption::Project: return "--project";
		case EOption::Json: return "--json";
		case EOption::All: return "--all";
		case EOption::Apply: return "--apply";
		case EOption::Scope: return "scope";
		}
		return "option";
	}

	struct FOptions
	{
		std::string Project;
		EOperation Operation = EOperation::Check;
		EOutputFormat Format = EOutputFormat::Human;
		bool bHelp = false;
		bool bWholeProject = false;
		bool bApply = false;
		std::vector<std::string> Scopes;
		uint16 SpecifiedOptions = 0;
	};

	auto PrintUsage() -> void
	{
		std::cout
			<< "Usage:\n"
			<< "  DurinAssetTool check --project=<project.dproject> [--json]\n"
			<< "  DurinAssetTool resave --project=<project.dproject> <scope>... [--apply] [--json]\n"
			<< "  DurinAssetTool resave --project=<project.dproject> --all [--apply] [--json]\n"
			<< "  DurinAssetTool storage-inventory --project=<project.dproject>\n";
	}

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
		if (Options.bHelp) return true;
		if (Options.Project.empty())
		{
			OutError = "--project=<path-to-project.dproject> is required.";
			return false;
		}

		constexpr uint16 Check = OptionBit(EOption::Project) | OptionBit(EOption::Json);
		constexpr uint16 Resave = Check | OptionBit(EOption::All)
			| OptionBit(EOption::Apply) | OptionBit(EOption::Scope);
		constexpr uint16 Storage = OptionBit(EOption::Project);
		const uint16 Allowed = Options.Operation == EOperation::Resave
			? Resave
			: Options.Operation == EOperation::Check ? Check : Storage;
		const uint16 Unexpected = Options.SpecifiedOptions & ~Allowed;
		constexpr EOption OrderedOptions[] = {
			EOption::Json, EOption::All, EOption::Apply, EOption::Scope};
		for (const EOption Option : OrderedOptions)
			if ((Unexpected & OptionBit(Option)) != 0)
			{
				OutError = std::format("{} is not valid for operation {}.",
					OptionName(Option), OperationName(Options.Operation));
				return false;
			}
		if (Options.Operation != EOperation::Resave) return true;
		if (Options.Scopes.empty() && !Options.bWholeProject)
		{
			OutError = "resave requires at least one scope or --all.";
			return false;
		}
		if (Options.bWholeProject && !Options.Scopes.empty())
		{
			OutError = "resave accepts either scopes or --all, not both.";
			return false;
		}
		return true;
	}

	auto ParseOptions(int ArgC, char** ArgV, FOptions& OutOptions, std::string& OutError) -> bool
	{
		if (ArgC < 2)
		{
			OutError = "a command is required; use --help for usage.";
			return false;
		}
		const std::string_view Command = ArgV[1];
		if (Command == "--help" || Command == "-h")
		{
			OutOptions.bHelp = true;
			return true;
		}
		if (Command == "check") OutOptions.Operation = EOperation::Check;
		else if (Command == "resave") OutOptions.Operation = EOperation::Resave;
		else if (Command == "storage-inventory")
			OutOptions.Operation = EOperation::StorageInventory;
		else
		{
			OutError = std::format("unknown command: {}", Command);
			return false;
		}

		for (int Index = 2; Index < ArgC; ++Index)
		{
			const std::string_view Argument = ArgV[Index];
			if (Argument == "--help" || Argument == "-h")
			{
				OutOptions.bHelp = true;
				continue;
			}
			if (Argument.starts_with("--project="))
			{
				if (!MarkOptionOnce(OutOptions, EOption::Project, OutError)) return false;
				OutOptions.Project = Argument.substr(std::string_view("--project=").size());
			}
			else if (Argument == "--all")
			{
				if (!MarkOptionOnce(OutOptions, EOption::All, OutError)) return false;
				OutOptions.bWholeProject = true;
			}
			else if (Argument == "--apply")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Apply, OutError)) return false;
				OutOptions.bApply = true;
			}
			else if (Argument == "--json")
			{
				if (!MarkOptionOnce(OutOptions, EOption::Json, OutError)) return false;
				OutOptions.Format = EOutputFormat::Json;
			}
			else if (!Argument.starts_with("-"))
			{
				OutOptions.SpecifiedOptions |= OptionBit(EOption::Scope);
				OutOptions.Scopes.emplace_back(Argument);
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
			.bWholeProject = Options.bWholeProject,
			.bAllowPlainResave = true};
		for (const std::string& Value : Options.Scopes)
		{
			if (!IsValidVirtualPrefix(Value))
			{
				OutError = std::format("invalid scope '{}'.", Value);
				return 2;
			}
			const bool bExact = std::ranges::any_of(Records, [&](const auto& Record) {
				return Record.PackagePath.ToString() == Value;
			});
			const bool bDescendant = std::ranges::any_of(Records, [&](const auto& Record) {
				return MatchesVirtualPrefix(Record, Value);
			});
			if (!bExact && !bDescendant)
			{
				OutError = std::format("scope '{}' matched no discovered package.", Value);
				return 1;
			}
			if (bExact)
			{
				Durin::FAssetPath Path;
				if (!Durin::FAssetPath::TryCreate(Value, Path, &OutError))
				{
					OutError = std::format("invalid scope '{}': {}", Value, OutError);
					return 2;
				}
				OutSelection.Packages.push_back(std::move(Path));
			}
			if (bDescendant) OutSelection.Folders.push_back(Value);
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
		return 0;
	}

	auto PrintCompatibilityCheck(
		std::span<const Durin::Asset::FAssetPackageCompatibilityRecord> Records) -> void
	{
		using namespace Durin::Asset;
		const auto Compatible = std::ranges::count(Records,
			EAssetPackageCompatibility::Compatible,
			&FAssetPackageCompatibilityRecord::Compatibility);
		const auto Incompatible = std::ranges::count(Records,
			EAssetPackageCompatibility::Incompatible,
			&FAssetPackageCompatibilityRecord::Compatibility);
		const auto Unsupported = std::ranges::count(Records,
			EAssetPackageCompatibility::Unsupported,
			&FAssetPackageCompatibilityRecord::Compatibility);
		const auto Failed = std::ranges::count(Records,
			EAssetCompatibilityInspection::Failed,
			&FAssetPackageCompatibilityRecord::Inspection);
		const auto Stale = std::ranges::count(Records,
			EAssetCompatibilityFreshness::Stale,
			&FAssetPackageCompatibilityRecord::Freshness);
		const auto ResaveRecommended = std::ranges::count_if(Records, [](const auto& Record) {
			return !Record.CanonicalizationEvidence.empty()
				|| !Record.DeprecatedRouteEvidence.empty();
		});
		std::cout << "asset check: " << Records.size() << " package(s); "
			<< Compatible << " compatible, " << Incompatible << " incompatible, "
			<< Unsupported << " unsupported, " << Failed << " failed, "
			<< Stale << " stale, " << ResaveRecommended << " resave recommended.\n";
		for (const FAssetPackageCompatibilityRecord& Record : Records)
		{
			const bool bNeedsAttention = Record.Compatibility != EAssetPackageCompatibility::Compatible
				|| Record.Inspection == EAssetCompatibilityInspection::Failed
				|| Record.Freshness == EAssetCompatibilityFreshness::Stale
				|| !Record.CanonicalizationEvidence.empty()
				|| !Record.DeprecatedRouteEvidence.empty();
			if (!bNeedsAttention) continue;
			std::cout << "  " << Record.PackagePath.ToString() << '\n';
			for (const FAssetCompatibilityFinding& Finding : Record.Findings)
				std::cout << "    " << Finding.Diagnostic << '\n';
			if (!Record.CanonicalizationEvidence.empty()
				|| !Record.DeprecatedRouteEvidence.empty())
				std::cout << "    canonical resave recommended\n";
		}
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
	if (Options.bHelp)
	{
		PrintUsage();
		return 0;
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
	// Project initialization configures logging. Keep stdout reserved for the
	// selected human or machine-readable report.
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
	struct FScopedEditorServices final
	{
		bool bStarted = false;
		~FScopedEditorServices()
		{
			if (!bStarted) return;
			Durin::ShutdownAssetCompilingManager();
			Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
		}
	} EditorServices;
#if DURIN_WITH_EDITOR
	if (!Durin::InitializeTaskScheduler(2)
		|| !Durin::InitializeGameThreadDeferredExecutor()
		|| !Durin::InitializeAssetCompilingManager())
	{
		std::cerr << "Error: asset inspection could not initialize editor task services.\n";
		return 1;
	}
	EditorServices.bStarted = true;
	{
		Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("SkeletalBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("TerrainBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	}
#endif
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
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
	if (Options.Operation == EOperation::StorageInventory)
	{
		std::cout << SerializeStorageQualificationInventory(Snapshot.Packages) << '\n';
		return 0;
	}
	if (Options.Operation == EOperation::Resave && Options.bApply)
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
	if (Options.Operation == EOperation::Check)
	{
		if (Options.Format == EOutputFormat::Json)
			std::cout << Durin::Asset::SerializeAssetCompatibilityReportV1(Records) << '\n';
		else
			PrintCompatibilityCheck(Records);
		return 0;
	}

	return RunCanonicalResave(Options, Records, Catalog);
}

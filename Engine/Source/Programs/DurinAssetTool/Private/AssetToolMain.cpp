#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Scan.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "Asset/AssetSaveReadiness.h"

#include "Asset/EditorBulkDataStorage.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageInspection.h"
#include "Asset/References.h"
#include "Asset/Cook.h"

#include "Animation/AnimationClip.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Level.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "HAL/PlatformMisc.h"
#include "Json/Json.h"
#include "Logging/Logger.h"
#include "Misc/FileHelper.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

#include <chrono>
#include <csignal>

namespace
{
	std::atomic_bool GCancelled = false;
	auto HandleInterrupt(int) -> void { GCancelled.store(true, std::memory_order_relaxed); }

	auto PrepareCanonicalResaveAsset(
		const Durin::FPackagePath& Path, Durin::DObject* Asset
	)
		-> Durin::FAssetResult
	{
		if (GCancelled.load(std::memory_order_relaxed))
			return {Durin::EAssetError::ShuttingDown, "Canonical resave was cancelled before asset compilation completed."};
		(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Asset);
		return Durin::ValidateAssetSaveReadiness(Asset);
	}

	enum class EOperation : uint8
	{
		Check,
		Resave,
		StorageInventory,
		IdentityAudit,
		Cook,
	};

	enum class EOutputFormat : uint8
	{
		Json,
		Human
	};

	enum class EOption : uint16
	{
		Project = 1 << 0,
		Json = 1 << 1,
		All = 1 << 2,
		Apply = 1 << 3,
		Scope = 1 << 4,
		Output = 1 << 5,
		Target = 1 << 6,
		Profile = 1 << 7,
		Root = 1 << 8,
		NoIncremental = 1 << 9,
		DryRun = 1 << 10,
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
		case EOperation::IdentityAudit: return "identity-audit";
		case EOperation::Cook: return "cook";
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
		case EOption::Output: return "--output";
		case EOption::Target: return "--target";
		case EOption::Profile: return "--profile";
		case EOption::Root: return "--root";
		case EOption::NoIncremental: return "--no-incremental";
		case EOption::DryRun: return "--dry-run";
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
		bool bIncremental = true;
		bool bDryRun = false;
		std::filesystem::path OutputRoot;
		std::string Target;
		std::string TargetProfile;
		std::vector<std::string> CookRoots;
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
			<< "  DurinAssetTool storage-inventory --project=<project.dproject>\n"
			<< "  DurinAssetTool identity-audit --project=<project.dproject>\n"
			<< "  DurinAssetTool cook --project=<project.dproject> --output=<absolute-path> "
			<< "--target=win64 --profile=game [--root=/Game/Path]... "
			<< "[--no-incremental] [--dry-run] [--json]\n";
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
		constexpr uint16 Cook = Check | OptionBit(EOption::Output)
								| OptionBit(EOption::Target) | OptionBit(EOption::Profile)
								| OptionBit(EOption::Root) | OptionBit(EOption::NoIncremental)
								| OptionBit(EOption::DryRun);
		const uint16 Allowed = Options.Operation == EOperation::Resave ? Resave : Options.Operation == EOperation::Check ? Check :
															  Options.Operation == EOperation::Cook		 ? Cook : Storage;
		const uint16 Unexpected = Options.SpecifiedOptions & ~Allowed;
		constexpr EOption OrderedOptions[] = {
			EOption::Json, EOption::All, EOption::Apply, EOption::Scope,
			EOption::Output, EOption::Target, EOption::Profile, EOption::Root,
			EOption::NoIncremental, EOption::DryRun
		};
		for (const EOption Option : OrderedOptions)
			if ((Unexpected & OptionBit(Option)) != 0)
			{
				OutError = std::format("{} is not valid for operation {}.", OptionName(Option), OperationName(Options.Operation));
				return false;
			}
		if (Options.Operation == EOperation::Cook)
		{
			if (Options.OutputRoot.empty())
			{
				OutError = "cook requires --output=<absolute-path>.";
				return false;
			}
			if (!Options.OutputRoot.is_absolute())
			{
				OutError = "cook --output must be absolute.";
				return false;
			}
			if (Options.Target != "win64" || Options.TargetProfile != "game")
			{
				OutError = "cook currently requires --target=win64 --profile=game.";
				return false;
			}
			return true;
		}
		if (Options.Operation != EOperation::Resave) return true;
		if (Options.Scopes.empty() && !Options.bWholeProject)
		{
			OutError = std::format("{} requires at least one scope or --all.",
				OperationName(Options.Operation));
			return false;
		}
		if (Options.bWholeProject && !Options.Scopes.empty())
		{
			OutError = std::format("{} accepts either scopes or --all, not both.",
				OperationName(Options.Operation));
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
		if (Command == "check")
			OutOptions.Operation = EOperation::Check;
		else if (Command == "resave")
			OutOptions.Operation = EOperation::Resave;
		else if (Command == "storage-inventory")
			OutOptions.Operation = EOperation::StorageInventory;
		else if (Command == "identity-audit")
			OutOptions.Operation = EOperation::IdentityAudit;
		else if (Command == "cook")
			OutOptions.Operation = EOperation::Cook;
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
			else if (Argument.starts_with("--output="))
			{
				if (!MarkOptionOnce(OutOptions, EOption::Output, OutError)) return false;
				OutOptions.OutputRoot = Argument.substr(std::string_view("--output=").size());
			}
			else if (Argument.starts_with("--target="))
			{
				if (!MarkOptionOnce(OutOptions, EOption::Target, OutError)) return false;
				OutOptions.Target = Argument.substr(std::string_view("--target=").size());
			}
			else if (Argument.starts_with("--profile="))
			{
				if (!MarkOptionOnce(OutOptions, EOption::Profile, OutError)) return false;
				OutOptions.TargetProfile = Argument.substr(std::string_view("--profile=").size());
			}
			else if (Argument.starts_with("--root="))
			{
				OutOptions.SpecifiedOptions |= OptionBit(EOption::Root);
				OutOptions.CookRoots.emplace_back(
					Argument.substr(std::string_view("--root=").size())
				);
			}
			else if (Argument == "--no-incremental")
			{
				if (!MarkOptionOnce(OutOptions, EOption::NoIncremental, OutError)) return false;
				OutOptions.bIncremental = false;
			}
			else if (Argument == "--dry-run")
			{
				if (!MarkOptionOnce(OutOptions, EOption::DryRun, OutError)) return false;
				OutOptions.bDryRun = true;
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

	struct FQualificationDescriptor
	{
		Durin::FEditorBulkDataStorageDescriptor Descriptor;
		std::filesystem::path CompanionPath;
		bool bReachable = true;
		std::string Diagnostic;
		Durin::FSharedByteBuffer ExternalBytes;
		uint64 ExactDuplicateGroup = 0;
	};

	auto SerializeStorageQualificationInventory(
		std::span<const Durin::FAssetPackageCompatibilityProbeInput> Inputs
	)
		-> std::string
	{
		using namespace Durin;
		using namespace Durin;
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
				Package.InspectionNanoseconds.push_back(static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(End - Start).count()));
				if (!Package.Result) break;
				Package.Inspection = std::move(Candidate);
			}
			if (Package.Result)
			{
				std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
				if (!InspectEditorBulkDataStorageDescriptors(
						Package.Inspection, Descriptors, &Package.DescriptorDiagnostic
					))
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
							Item.CompanionPath = Input.PhysicalPath;
							Item.CompanionPath.replace_extension(".dbulk");
							FByteArray Segment;
							if (!FFileHelper::LoadFileToArray(Segment, Item.CompanionPath)
								|| Descriptor.SegmentOffset > Segment.size()
								|| Descriptor.StoredByteCount
									> Segment.size() - Descriptor.SegmentOffset)
							{
								Item.Diagnostic = "Package bulk field range is missing or unreadable.";
								Item.bReachable = false;
							}
							else
							{
								const auto Bytes = std::span<const std::byte>(Segment).subspan(
									static_cast<size_t>(Descriptor.SegmentOffset),
									static_cast<size_t>(Descriptor.StoredByteCount));
								if (FXxHash128::HashBuffer(Bytes) != Descriptor.ContentHash)
								{
									Item.Diagnostic = "Package bulk field content identity is invalid.";
									Item.bReachable = false;
								}
								else
									Item.ExternalBytes = FSharedByteBuffer::Copy(Bytes);
							}
						}
						Package.Descriptors.push_back(std::move(Item));
					}
					std::string OrphanError;
					if (!InspectOrphanedEditorBulkDataCompanionPaths(
							Input.PhysicalPath, Package.Inspection, Package.Orphans, &OrphanError
						))
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

		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", 2);
		Root.SetChildValue("inspectionRepeatCount", 5);
		FJsonNodeRef PackageArray = Root.AddArray("packages");
		for (const FPackage& Package : Packages)
		{
			FJsonNodeRef PackageNode = PackageArray.AppendObject();
			PackageNode.SetChildValue("packagePath", Package.Input->PackagePath.GetView());
			PackageNode.SetChildValue("physicalPath", Package.Input->PhysicalPath);
			PackageNode.SetChildValue("fileSize", static_cast<uint64>(Package.Input->ExpectedFileSize));
			PackageNode.SetChildValue("inspection", Package.Result ? "Ready" : "Failed");
			PackageNode.SetChildValue("diagnostic", Package.Result.Message);
			PackageNode.SetChildValue("formatVersion", Package.Inspection.Header.FormatVersion);
			PackageNode.SetChildValue("fileBytesRead", static_cast<uint64>(Package.Input->ExpectedFileSize));

			FJsonNodeRef InspectionTimes = PackageNode.AddArray("inspectionNanoseconds");
			for (const uint64 Nanoseconds : Package.InspectionNanoseconds)
				InspectionTimes.AppendValue(Nanoseconds);

			FJsonNodeRef DescriptorArray = PackageNode.AddArray("descriptors");
			for (const FQualificationDescriptor& Item : Package.Descriptors)
			{
				const auto& Descriptor = Item.Descriptor;
				FJsonNodeRef DescriptorNode = DescriptorArray.AppendObject();
				DescriptorNode.SetChildValue("payloadId", Descriptor.PayloadId.ToString());
				DescriptorNode.SetChildValue("logicalBytes", Descriptor.LogicalByteCount);
				DescriptorNode.SetChildValue("storedBytes", Descriptor.StoredByteCount);
				DescriptorNode.SetChildValue("contentHash", Descriptor.ContentHash.ToString());
				DescriptorNode.SetChildValue("containerHash", Descriptor.ContainerHash.ToString());
				DescriptorNode.SetChildValue("storage",
					Descriptor.StorageKind == EEditorBulkDataStorageKind::Inline ? "Inline" : "External");
				DescriptorNode.SetChildValue("companionPath", Item.CompanionPath.generic_string());
				DescriptorNode.SetChildValue("reachable", Item.bReachable);
				DescriptorNode.SetChildValue("diagnostic", Item.Diagnostic);
				DescriptorNode.SetChildValue("exactDuplicateGroup", Item.ExactDuplicateGroup);
			}

			FJsonNodeRef OrphanArray = PackageNode.AddArray("orphanCompanions");
			for (const std::filesystem::path& Orphan : Package.Orphans)
				OrphanArray.AppendValue(Orphan.generic_string());
			PackageNode.SetChildValue("descriptorDiagnostic", Package.DescriptorDiagnostic);
		}
		return Document.ToString();
	}

	auto SerializeIdentityAudit(
		std::span<const Durin::FAssetPackageCompatibilityProbeInput> Inputs
	) -> std::string
	{
		using namespace Durin;
		using namespace Durin;
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", 1);
		FJsonNodeRef Packages = Root.AddArray("packages");
		for (const FAssetPackageCompatibilityProbeInput& Input : Inputs)
		{
			FJsonNodeRef PackageNode = Packages.AppendObject();
			PackageNode.SetChildValue("packagePath", Input.PackagePath.GetView());
			FAssetPackageInspection Inspection;
			const FAssetResult Result = InspectAssetPackage(
				Input.PhysicalPath, Input.PackagePath, Inspection);
			PackageNode.SetChildValue("inspection", Result ? "Ready" : "Failed");
			PackageNode.SetChildValue("diagnostic", Result.Message);
			if (!Result) continue;
			PackageNode.SetChildValue("formatVersion", Inspection.Header.FormatVersion);
			PackageNode.SetChildValue("assetClass", Inspection.Header.AssetClassName);
			PackageNode.SetChildValue("redirectDestination",
				Inspection.Header.RedirectDestination.ToString());

			FJsonNodeRef Objects = PackageNode.AddArray("objects");
			for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			{
				FJsonNodeRef ObjectNode = Objects.AppendObject();
				ObjectNode.SetChildValue("id", Object.Id);
				ObjectNode.SetChildValue("outerId", Object.OuterId);
				ObjectNode.SetChildValue("name", Object.ObjectName);
				ObjectNode.SetChildValue("class", Object.ClassName);
				ObjectNode.SetChildValue("path", Object.ObjectPath);
			}

			std::vector<FAssetReferenceEdge> References;
			const FAssetResult ReferenceResult = ExtractAssetReferences(
				Input.PackagePath, Inspection, References);
			PackageNode.SetChildValue("referenceInspection",
				ReferenceResult ? "Ready" : "Failed");
			PackageNode.SetChildValue("referenceDiagnostic", ReferenceResult.Message);
			FJsonNodeRef ReferenceArray = PackageNode.AddArray("references");
			if (ReferenceResult)
			{
				for (const FAssetReferenceEdge& Reference : References)
				{
					FJsonNodeRef ReferenceNode = ReferenceArray.AppendObject();
					ReferenceNode.SetChildValue("kind",
						Reference.Kind == EAssetReferenceKind::HardObject ? "Hard"
						: Reference.Kind == EAssetReferenceKind::SoftObject ? "Soft" : "Redirect");
					ReferenceNode.SetChildValue("sourceObjectId", Reference.SourceObjectId);
					ReferenceNode.SetChildValue("route", Reference.DisplayRoute);
					ReferenceNode.SetChildValue("target", Reference.TargetPath.ToString());
				}
			}
		}
		return Document.ToString();
	}

	auto IsValidVirtualPrefix(std::string_view Value) -> bool
	{
		return Value.size() >= 2 && Value.front() == '/' && Value.back() != '/'
			   && Value.find("//") == std::string_view::npos
			   && Value.find('\\') == std::string_view::npos;
	}

	auto MatchesVirtualPrefix(
		const Durin::FAssetPackageCompatibilityRecord& Record,
		std::string_view Prefix
	) -> bool
	{
		const std::string_view Path = Record.PackagePath.GetView();
		return Path.starts_with(Prefix) && Path.size() > Prefix.size()
			   && Path[Prefix.size()] == '/';
	}

	auto MakeCanonicalResaveSelection(
		const FOptions& Options,
		std::span<const Durin::FAssetPackageCompatibilityRecord> Records,
		Durin::FAssetCanonicalResaveSelection& OutSelection,
		std::string& OutError
	) -> int
	{
		OutSelection = {
			.bWholeProject = Options.bWholeProject,
			.bAllowPlainResave = true
		};
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
				Durin::FPackagePath Path;
				if (!Durin::FPackagePath::TryCreate(Value, Path, &OutError))
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
		std::span<const Durin::FAssetPackageCompatibilityRecord> Records,
		const Durin::FReflectionCompatibilityCatalog& Catalog
	) -> int
	{
		std::string Error;
		Durin::FAssetCanonicalResaveSelection Selection;
		if (const int Result = MakeCanonicalResaveSelection(
				Options, Records, Selection, Error
			))
		{
			std::cerr << "Error: " << Error << '\n';
			return Result;
		}
		const auto Plan = Durin::PlanAssetCanonicalResaves(
			Records, Selection, [] { return GCancelled.load(std::memory_order_relaxed); }
		);
		if (Plan.Status == Durin::EAssetCanonicalResavePlanStatus::Cancelled)
			return 130;
		if (Options.bApply)
		{
			auto Applied = Durin::ApplyAssetCanonicalResaves(
				Plan, Catalog,
				{.PrepareLoadedAsset = PrepareCanonicalResaveAsset},
				[] { return GCancelled.load(std::memory_order_relaxed); }
			);
			if (Options.Format == EOutputFormat::Human)
				std::cout << "canonical-resave apply: " << Applied.ChangedPaths.size()
						  << " package(s) resaved; " << Applied.Diagnostic << '\n';
			else
				std::cout << Durin::SerializeAssetCanonicalResaveApplyReport(Applied)
						  << '\n';
			std::cout.flush();
			if (Applied.Status == Durin::EAssetCanonicalResaveApplyStatus::Cancelled)
				return 130;
			return Applied.Status == Durin::EAssetCanonicalResaveApplyStatus::Succeeded ? 0 : 1;
		}
		if (Options.Format == EOutputFormat::Human)
		{
			const auto Ready = std::ranges::count(Plan.Packages, Durin::EAssetCanonicalResavePackageStatus::Ready, &Durin::FAssetCanonicalResavePackagePlan::Status);
			const auto Blocked = std::ranges::count(Plan.Packages, Durin::EAssetCanonicalResavePackageStatus::Blocked, &Durin::FAssetCanonicalResavePackagePlan::Status);
			const auto Skipped = std::ranges::count(Plan.Packages, Durin::EAssetCanonicalResavePackageStatus::Skipped, &Durin::FAssetCanonicalResavePackagePlan::Status);
			std::cout << "canonical-resave plan: " << Ready << " ready, "
					  << Blocked << " blocked, " << Skipped << " skipped, "
					  << Plan.Packages.size() << " selected\n";
			for (const auto& Package : Plan.Packages)
				if (Package.Status == Durin::EAssetCanonicalResavePackageStatus::Blocked)
				{
					std::cout << "  " << Package.PackagePath.ToString() << '\n';
					for (const std::string& Diagnostic : Package.Diagnostics)
						std::cout << "    " << Diagnostic << '\n';
				}
		}
		else
			std::cout << Durin::SerializeAssetCanonicalResavePlanReport(Plan) << '\n';
		return 0;
	}

	auto PrintCompatibilityCheck(
		std::span<const Durin::FAssetPackageCompatibilityRecord> Records
	) -> void
	{
		using namespace Durin;
		const auto Compatible = std::ranges::count(Records, EAssetPackageCompatibility::Compatible, &FAssetPackageCompatibilityRecord::Compatibility);
		const auto Incompatible = std::ranges::count(Records, EAssetPackageCompatibility::Incompatible, &FAssetPackageCompatibilityRecord::Compatibility);
		const auto Unsupported = std::ranges::count(Records, EAssetPackageCompatibility::Unsupported, &FAssetPackageCompatibilityRecord::Compatibility);
		const auto Failed = std::ranges::count(Records, EAssetCompatibilityInspection::Failed, &FAssetPackageCompatibilityRecord::Inspection);
		const auto Stale = std::ranges::count(Records, EAssetCompatibilityFreshness::Stale, &FAssetPackageCompatibilityRecord::Freshness);
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

	auto RegisterCookContributors(
		std::vector<Durin::FCookContributorHandle>& OutHandles,
		std::string& OutError
	) -> bool
	{
		using namespace Durin;
		using namespace Durin;
		OutHandles.clear();
		const FCookContributorHandle Generic = RegisterCookContributor(
			DObject::StaticClass(), {"generic-package", 1, 1,
									 [](DObject& Object, std::string_view VirtualPath,
										FCookContext& Context) -> FAssetResult {
										 std::string Error;
										 if (!Context.AddPackage(std::string(VirtualPath), Object.GetPackage(), &Error))
											 return {EAssetError::InvalidPackageType, std::move(Error)};
										 return {};
									 }}
		);
		if (Generic != 0) OutHandles.push_back(Generic);
		const bool bRegistered = Generic != 0
			&& RegisterEngineCookContributors(OutHandles, OutError);
		if (bRegistered)
		{
			OutError.clear();
			return true;
		}
		for (const FCookContributorHandle Handle : OutHandles)
			UnregisterCookContributor(Handle);
		OutHandles.clear();
		OutError = "CookContributorRegistrationFailed: a class has a duplicate or invalid contributor.";
		return false;
	}

	auto SerializeCookRunResult(const Durin::FCookRunResult& Result)
		-> std::string
	{
		using namespace Durin;
		using namespace Durin;
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", 1);
		Root.SetChildValue("status", CookRunStatusName(Result.Status));
		Root.SetChildValue("code", Result.Code);
		Root.SetChildValue("diagnostic", Result.Diagnostic);
		Root.SetChildValue("target", "win64");
		Root.SetChildValue("profile", "game");
		Root.SetChildValue("changedBytes", Result.ChangedBytes);
		Root.SetChildValue("reusedBytes", Result.ReusedBytes);
		Root.SetChildValue("peakCapturedBytes", Result.PeakCapturedBytes);
		Root.SetChildValue("rangeReadCount", Result.RangeReadCount);
		Root.SetChildValue("wallTimeNanoseconds", Result.WallTimeNanoseconds);
		Root.SetChildValue("commitTimeNanoseconds", Result.CommitTimeNanoseconds);
		Root.SetChildValue("rollbackTimeNanoseconds", Result.RollbackTimeNanoseconds);
		FJsonNodeRef PackageArray = Root.AddArray("packages");
		for (const FCookPackageResult& Package : Result.Packages)
		{
			FJsonNodeRef PackageNode = PackageArray.AppendObject();
			PackageNode.SetChildValue("packagePath", Package.PackagePath.GetView());
			PackageNode.SetChildValue("contributor", Package.Contributor);
			PackageNode.SetChildValue("status", CookPackageStatusName(Package.Status));
			PackageNode.SetChildValue("stage", CookOperationStageName(Package.Stage));
			PackageNode.SetChildValue("code", Package.Code);
			PackageNode.SetChildValue("diagnostic", Package.Diagnostic);
			PackageNode.SetChildValue("packageBytes", Package.PackageBytes);
			PackageNode.SetChildValue("segmentBytes", Package.SegmentBytes);
		}
		return Document.ToString();
	}

	auto RunCook(const FOptions& Options) -> int
	{
		using namespace Durin;
		using namespace Durin;
		std::vector<FPackagePath> Roots;
		for (const std::string& Value : Options.CookRoots)
		{
			FPackagePath Path;
			std::string Error;
			if (!FPackagePath::TryCreate(Value, Path, &Error))
			{
				std::cerr << "Error: invalid Cook root '" << Value << "': " << Error << '\n';
				return 2;
			}
			Roots.push_back(std::move(Path));
		}
		const FAssetCatalogRefreshResult Refresh = RefreshAssetRegistry(
			EAssetRegistryScanMode::FullValidation
		);
		if (!Refresh || !Refresh.bPublished)
		{
			std::cerr << "Error: Cook requires a complete published asset registry.\n";
			return 1;
		}
		std::vector<FCookContributorHandle> Handles;
		std::string Error;
		if (!RegisterCookContributors(Handles, Error))
		{
			std::cerr << "Error: " << Error << '\n';
			return 1;
		}
		struct FContributorCleanup
		{
			std::vector<FCookContributorHandle>& Handles;
			~FContributorCleanup()
			{
				for (const FCookContributorHandle Handle : Handles)
					UnregisterCookContributor(Handle);
			}
		} Cleanup{Handles};
		FCookCoordinator Coordinator;
		FCookRunResult Result;
		const FCookRequest Request{
			.OutputRoot = Options.OutputRoot,
			.TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game,
			.ExplicitRoots = std::move(Roots),
			.IncrementalPolicy = Options.bIncremental ? ECookIncrementalPolicy::Enabled : ECookIncrementalPolicy::Disabled,
			.bDryRun = Options.bDryRun,
			.IsCancelled = [] { return GCancelled.load(std::memory_order_relaxed); }
		};
		(void)Coordinator.Run(Request, Result);
		if (Options.Format == EOutputFormat::Json)
			std::cout << SerializeCookRunResult(Result) << '\n';
		else
		{
			const auto Hits = std::ranges::count(Result.Packages, ECookPackageStatus::CookHit, &FCookPackageResult::Status);
			std::cout << "cook " << CookRunStatusName(Result.Status) << ": "
					  << Result.Packages.size() << " package(s), " << Hits
					  << " Cook hit(s), " << Result.ChangedBytes << " changed byte(s), "
					  << Result.ReusedBytes << " reused byte(s).\n";
			if (!Result.Diagnostic.empty())
				std::cout << "  " << Result.Code << ": " << Result.Diagnostic << '\n';
		}
		if (Result.Status == ECookRunStatus::Cancelled) return 130;
		return Result.Status == ECookRunStatus::Succeeded ? 0 : 1;
	}
} // namespace

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
			{.RequestedProjectFile = Options.Project}, &Error
		))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::FPlatformMisc::EnableUserBinaryDirectoriesSearch();
	Durin::FPlatformMisc::AddRuntimeBinaryDirectory(
		Durin::FPaths::EngineThirdPartyRuntimeBinariesDir().c_str()
	);
	// Project initialization configures logging. Keep stdout reserved for the
	// selected human or machine-readable report.
	Durin::LoggerInit();
	struct FScopedLoggerShutdown final
	{
		~FScopedLoggerShutdown() { Durin::LoggerShutdown(); }
	} ScopedLoggerShutdown;
	Durin::FLogger::Get().SetConsoleLogLevel(Durin::ELogLevel::Fatal);
	if (!Durin::FMountPaths::InitDefaultMountPoints(&Error))
	{
		std::cerr << "Error: " << Error << '\n';
		return 1;
	}
	Durin::DObjectInit();
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
	struct FScopedShaderInventoryModule final
	{
		Durin::FModuleHandle Handle = nullptr;
		~FScopedShaderInventoryModule()
		{
			if (Handle) Durin::FPlatformMisc::FreeLibrary(Handle);
		}
	} ShaderInventoryModule;
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
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
		if (Options.Operation == EOperation::Cook)
		{
			Durin::FModuleManager::Get().LoadModuleChecked("RenderCore");
			ShaderInventoryModule.Handle = Durin::FPlatformMisc::LoadLibrary(
				std::format("{}{}-Renderer{}", Durin::FPlatformMisc::FLibraryPrefix,
					DURIN_RUNTIME_VARIANT,
					Durin::FPlatformMisc::FLibraryExtension));
			if (!ShaderInventoryModule.Handle)
			{
				std::cerr << "Error: Renderer Shader inventory could not load: "
					<< Durin::FPlatformMisc::GetLastLibraryError() << '\n';
				return 1;
			}
			Durin::FModuleManager::Get().LoadModuleChecked("ShaderBuild");
		}
	}
#endif
	(void)Durin::DLevel::StaticClass(); // Force the Engine reflection module into this process.
	if (Options.Operation == EOperation::Cook) return RunCook(Options);
	const Durin::FReflectionCompatibilityCatalog Catalog =
		Durin::FReflectionCompatibilityCatalog::Capture();
	Durin::FAssetPackageDiscoverySnapshot Snapshot =
		Durin::CaptureMountedAssetPackageSnapshot(
			[] { return GCancelled.load(std::memory_order_relaxed); }
		);
	if (Snapshot.Status == Durin::EAssetPackageSnapshotStatus::Cancelled) return 130;
	if (Snapshot.Status == Durin::EAssetPackageSnapshotStatus::Failed)
	{
		std::cerr << "Error: " << Snapshot.Error << '\n';
		return 1;
	}
	if (Options.Operation == EOperation::StorageInventory)
	{
		std::cout << SerializeStorageQualificationInventory(Snapshot.Packages) << '\n';
		return 0;
	}
	if (Options.Operation == EOperation::IdentityAudit)
	{
		std::cout << SerializeIdentityAudit(Snapshot.Packages) << '\n';
		return 0;
	}
	if (Options.Operation == EOperation::Resave && Options.bApply)
	{
		const Durin::FAssetCatalogRefreshResult Refresh =
			Durin::RefreshAssetRegistry(
				Durin::EAssetRegistryScanMode::FullValidation
			);
		if (!Refresh || !Refresh.bPublished)
		{
			std::cerr << "Error: canonical-resave requires a complete published asset catalog.\n";
			return 1;
		}
	}
	if (Options.Operation == EOperation::Resave)
		for (auto& Input : Snapshot.Packages)
			Input.bIncludeNestedMigrationEvidence = true;

	auto Audit = Durin::RunAssetCompatibilityAudit(
		Snapshot.Packages, Catalog,
		[] { return GCancelled.load(std::memory_order_relaxed); });
	if (Audit.Status == Durin::EAssetCompatibilityAuditStatus::Cancelled) return 130;
	auto& Records = Audit.Records;
	if (Options.Operation == EOperation::Check)
	{
		if (Options.Format == EOutputFormat::Json)
			std::cout << Durin::SerializeAssetCompatibilityReport(Records) << '\n';
		else
			PrintCompatibilityCheck(Records);
		return 0;
	}

	return RunCanonicalResave(Options, Records, Catalog);
}

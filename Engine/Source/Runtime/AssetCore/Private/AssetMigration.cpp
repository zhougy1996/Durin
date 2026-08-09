#include "AssetMigration.h"
#include "AssetPackageCodec.h"
#include "AssetSystem.h"

#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Durin::Asset
{
	namespace
	{
		auto JsonEscape(std::string_view Value) -> std::string
		{
			std::string Result;
			for (const unsigned char Character : Value)
			{
				switch (Character)
				{
				case '\\': Result += "\\\\"; break;
				case '"': Result += "\\\""; break;
				case '\b': Result += "\\b"; break;
				case '\f': Result += "\\f"; break;
				case '\n': Result += "\\n"; break;
				case '\r': Result += "\\r"; break;
				case '\t': Result += "\\t"; break;
				default:
					if (Character < 0x20) Result += std::format("\\u{:04x}", Character);
					else Result.push_back(static_cast<char>(Character));
				}
			}
			return Result;
		}

		auto KindName(EAssetMigrationKind Kind) -> std::string_view
		{
			return Kind == EAssetMigrationKind::PackageFormat ? "PackageFormat" : "Unknown";
		}

		auto RiskName(EAssetMigrationRisk Risk) -> std::string_view
		{
			switch (Risk)
			{
			case EAssetMigrationRisk::Lossless: return "Lossless";
			case EAssetMigrationRisk::DataLoss: return "DataLoss";
			case EAssetMigrationRisk::Unknown: return "Unknown";
			}
			return "Unknown";
		}

		auto StatusName(EAssetMigrationPackageStatus Status) -> std::string_view
		{
			switch (Status)
			{
			case EAssetMigrationPackageStatus::Planned: return "Planned";
			case EAssetMigrationPackageStatus::Migrated: return "Migrated";
			case EAssetMigrationPackageStatus::Skipped: return "Skipped";
			case EAssetMigrationPackageStatus::Blocked: return "Blocked";
			case EAssetMigrationPackageStatus::Failed: return "Failed";
			case EAssetMigrationPackageStatus::RolledBack: return "RolledBack";
			}
			return "Failed";
		}

		auto IsSelected(const FAssetPackageCompatibilityRecord& Record, const FAssetMigrationSelection& Selection) -> bool
		{
			if (Selection.Mounts.empty() && Selection.Packages.empty()) return true;
			if (std::ranges::find(Selection.Packages, Record.PackagePath) != Selection.Packages.end()) return true;
			for (const std::string& Mount : Selection.Mounts)
			{
				std::string_view Prefix = Mount;
				while (Prefix.size() > 1 && Prefix.ends_with('/')) Prefix.remove_suffix(1);
				const std::string_view Path = Record.PackagePath.GetView();
				if (Path == Prefix || (Path.starts_with(Prefix) && Path.size() > Prefix.size() && Path[Prefix.size()] == '/')) return true;
			}
			return false;
		}
	}

	auto FAssetMigrationRegistry::Register(FAssetMigrationHandlerDescriptor Handler, std::string& OutError) -> bool
	{
		if (Handler.HandlerId.empty())
		{
			OutError = "MigrationHandlerInvalid: handler id is empty.";
			return false;
		}
		if (Handler.SourceVersion == Handler.TargetVersion)
		{
			OutError = std::format("MigrationHandlerInvalid: handler {} has a self edge at version {}.", Handler.HandlerId, Handler.SourceVersion);
			return false;
		}
		if (Handler.SourceCodecId.empty() || Handler.TargetCodecId.empty())
		{
			OutError = std::format(
				"MigrationHandlerInvalid: handler {} has no exact codec identity.",
				Handler.HandlerId);
			return false;
		}
		if (std::ranges::find(Handlers, Handler.HandlerId, &FAssetMigrationHandlerDescriptor::HandlerId) != Handlers.end())
		{
			OutError = std::format("MigrationHandlerDuplicateId: handler id {} is already registered.", Handler.HandlerId);
			return false;
		}
		Handlers.push_back(std::move(Handler));
		std::ranges::sort(Handlers, [](const auto& Left, const auto& Right) {
			return std::tie(Left.Kind, Left.SourceVersion, Left.TargetVersion, Left.HandlerId)
				< std::tie(Right.Kind, Right.SourceVersion, Right.TargetVersion, Right.HandlerId);
		});
		return true;
	}

	auto FAssetMigrationRegistry::Validate(std::string& OutError) const -> bool
	{
		for (size_t Index = 1; Index < Handlers.size(); ++Index)
		{
			const auto& Previous = Handlers[Index - 1];
			const auto& Current = Handlers[Index];
			if (Previous.Kind == Current.Kind
				&& Previous.SourceVersion == Current.SourceVersion
				&& Previous.TargetVersion == Current.TargetVersion)
			{
				OutError = std::format("MigrationEdgeAmbiguous: {} exact edge {} to {} has handlers {} and {}.",
					KindName(Current.Kind), Current.SourceVersion, Current.TargetVersion,
					Previous.HandlerId, Current.HandlerId);
				return false;
			}
		}
		return true;
	}

	auto FAssetMigrationRegistry::ResolveExactEdge(
		EAssetMigrationKind Kind,
		uint32 SourceVersion,
		uint32 TargetVersion,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested) const
		-> FAssetMigrationEdgeResolution
	{
		if (SourceVersion == TargetVersion) return {.Status = EAssetMigrationResolutionStatus::Resolved};
		std::string ValidationError;
		if (!Validate(ValidationError))
		{
			return {
				.Status = EAssetMigrationResolutionStatus::AmbiguousEdge,
				.Diagnostic = std::move(ValidationError)};
		}
		if (IsCancellationRequested && IsCancellationRequested())
			return {.Status = EAssetMigrationResolutionStatus::Cancelled,
				.Diagnostic = "MigrationCancelled: exact-edge resolution was cancelled."};
		const auto It = std::ranges::find_if(Handlers, [&](const auto& Handler) {
			return Handler.Kind == Kind && Handler.SourceVersion == SourceVersion
				&& Handler.TargetVersion == TargetVersion;
		});
		if (It == Handlers.end())
			return {.Status = EAssetMigrationResolutionStatus::MissingEdge,
				.Diagnostic = std::format(
					"MigrationEdgeMissing: no exact {} handler migrates version {} to {}.",
					KindName(Kind), SourceVersion, TargetVersion)};
		return {.Status = EAssetMigrationResolutionStatus::Resolved, .Steps = {*It}};
	}

	auto RegisterBuiltInAssetMigrations(FAssetMigrationRegistry& Registry, std::string& OutError) -> bool
	{
		return ValidateAssetPackageVersionPolicy(OutError) && Registry.Validate(OutError);
	}

	auto PlanAssetPackageMigrations(
		std::span<const FAssetPackageCompatibilityRecord> Records,
		const FAssetMigrationRegistry& Registry,
		const FAssetMigrationSelection& Selection,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested) -> FAssetMigrationPlan
	{
		FAssetMigrationPlan Result;
		std::vector<const FAssetPackageCompatibilityRecord*> Sorted;
		for (const auto& Record : Records) if (IsSelected(Record, Selection)) Sorted.push_back(&Record);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) { return Left->PackagePath.GetView() < Right->PackagePath.GetView(); });
		for (const auto* Record : Sorted)
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Result.Status = EAssetMigrationPlanStatus::Cancelled;
				Result.Packages.clear();
				return Result;
			}
			FAssetMigrationPackagePlan Package{
				.PackagePath = Record->PackagePath,
				.PhysicalPath = Record->PhysicalPath,
				.Fingerprint = Record->Fingerprint,
				.ReportContentHash = Record->ReportContentHash,
				.SourceFormatVersion = Record->FormatVersion,
				.ReaderPolicyFingerprint = GetAssetPackageReaderPolicyIdentity()};
			if (Record->Inspection == EAssetCompatibilityInspection::Failed)
			{
				Package.Status = EAssetMigrationPackageStatus::Failed;
				Package.Diagnostics.push_back("CompatibilityInspectionFailed: package metadata could not be safely inspected.");
			}
			else if (Record->Freshness != EAssetCompatibilityFreshness::Current)
			{
				Package.Status = EAssetMigrationPackageStatus::Blocked;
				Package.Diagnostics.push_back("StaleFingerprint: package changed after discovery.");
			}
			else if (Record->ReportContentHash.empty())
			{
				Package.Status = EAssetMigrationPackageStatus::Blocked;
				Package.Diagnostics.push_back("MissingContentHash: package has no stable SHA-256 report fingerprint.");
			}
			else if (!Record->Findings.empty() || Record->Compatibility != EAssetPackageCompatibility::Compatible)
			{
				Package.Status = EAssetMigrationPackageStatus::Blocked;
				for (const auto& Finding : Record->Findings)
					Package.Diagnostics.push_back(std::format("CompatibilityFinding:{}: {}", AssetCompatibilityFindingCodeName(Finding.Code), Finding.Diagnostic));
				if (Package.Diagnostics.empty()) Package.Diagnostics.push_back("CompatibilityBlocked: package is not losslessly compatible.");
			}
			else if (Record->FormatVersion == AssetPackageMigrationWriterVersion)
			{
				Package.Status = EAssetMigrationPackageStatus::Skipped;
			}
			else
			{
				auto Chain = Registry.ResolveExactEdge(EAssetMigrationKind::PackageFormat, Record->FormatVersion,
					AssetPackageMigrationWriterVersion, IsCancellationRequested);
				if (Chain.Status == EAssetMigrationResolutionStatus::Cancelled)
				{
					Result.Status = EAssetMigrationPlanStatus::Cancelled;
					Result.Packages.clear();
					return Result;
				}
				if (Chain.Status != EAssetMigrationResolutionStatus::Resolved)
				{
					Package.Status = EAssetMigrationPackageStatus::Blocked;
					Package.Diagnostics.push_back(std::move(Chain.Diagnostic));
				}
				else if (std::ranges::any_of(Chain.Steps, [](const auto& Step) { return Step.Risk != EAssetMigrationRisk::Lossless; }))
				{
					Package.Status = EAssetMigrationPackageStatus::Blocked;
					Package.Diagnostics.push_back("MigrationNotLossless: the exact edge has data-loss or unknown risk.");
				}
				else
				{
					const Private::FAssetPackageCodec* SourceCodec =
						Private::FindAssetPackageReader(Record->FormatVersion);
					const Private::FAssetPackageCodec* TargetCodec =
						Private::FindAssetPackageWriter(AssetPackageMigrationWriterVersion);
					if (!SourceCodec || !TargetCodec || Chain.Steps.size() != 1
						|| Chain.Steps.front().SourceCodecId != SourceCodec->CodecId
						|| Chain.Steps.front().TargetCodecId != TargetCodec->CodecId)
					{
						Package.Status = EAssetMigrationPackageStatus::Blocked;
						Package.Diagnostics.push_back(
							"MigrationCodecMismatch: exact edge codec identities do not match policy.");
					}
					else
					{
						Package.Status = EAssetMigrationPackageStatus::Planned;
						Package.Steps = std::move(Chain.Steps);
					}
				}
			}
			Result.Packages.push_back(std::move(Package));
		}

		bool bDependencyStatusChanged = true;
		while (bDependencyStatusChanged)
		{
			bDependencyStatusChanged = false;
			for (auto& Package : Result.Packages)
			{
				if (Package.Status != EAssetMigrationPackageStatus::Planned) continue;
				const auto RecordIt = std::ranges::find(Records, Package.PackagePath, &FAssetPackageCompatibilityRecord::PackagePath);
				if (RecordIt == Records.end()) continue;
				for (const FAssetPath& Dependency : RecordIt->Dependencies)
				{
					const auto DependencyRecord = std::ranges::find(Records, Dependency, &FAssetPackageCompatibilityRecord::PackagePath);
					if (DependencyRecord == Records.end() || DependencyRecord->FormatVersion == AssetPackageMigrationWriterVersion) continue;
					const auto DependencyPlan = std::ranges::find(Result.Packages, Dependency, &FAssetMigrationPackagePlan::PackagePath);
					if (DependencyPlan == Result.Packages.end())
						Package.Diagnostics.push_back(std::format("DependencyNotSelected: dependency {} also requires migration.", Dependency.ToString()));
					else if (DependencyPlan->Status != EAssetMigrationPackageStatus::Planned)
						Package.Diagnostics.push_back(std::format("DependencyBlocked: dependency {} has no ready lossless plan.", Dependency.ToString()));
					else continue;
					Package.Status = EAssetMigrationPackageStatus::Blocked;
					Package.Steps.clear();
					bDependencyStatusChanged = true;
					break;
				}
			}
		}
		return Result;
	}

	auto SerializeAssetMigrationPlanReportV2(const FAssetMigrationPlan& Plan) -> std::string
	{
		uint64 Planned = 0, Skipped = 0, Blocked = 0, Failed = 0;
		for (const auto& Package : Plan.Packages)
		{
			Planned += Package.Status == EAssetMigrationPackageStatus::Planned;
			Skipped += Package.Status == EAssetMigrationPackageStatus::Skipped;
			Blocked += Package.Status == EAssetMigrationPackageStatus::Blocked;
			Failed += Package.Status == EAssetMigrationPackageStatus::Failed;
		}
		const std::string_view ResultName = Blocked != 0 || Failed != 0 ? "Blocked" : "Ready";
		std::string Json = std::format("{{\"schemaVersion\":{},\"operation\":\"Plan\",\"result\":\"{}\",\"packages\":[", AssetMigrationReportSchemaVersion, ResultName);
		for (size_t Index = 0; Index < Plan.Packages.size(); ++Index)
		{
			if (Index != 0) Json += ',';
			const auto& Package = Plan.Packages[Index];
			Json += std::format("{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"status\":\"{}\",\"fingerprint\":{{\"fileSize\":{},\"lastWriteTimeTicks\":{},\"contentHash\":\"{}\"}},\"sourceFormatVersion\":{},\"targetFormatVersion\":{},\"readerPolicyFingerprint\":{},\"steps\":[",
				JsonEscape(Package.PackagePath.GetView()), JsonEscape(Package.PhysicalPath), StatusName(Package.Status),
				Package.Fingerprint.FileSize, Package.Fingerprint.LastWriteTimeTicks, JsonEscape(Package.ReportContentHash),
				Package.SourceFormatVersion, Package.TargetFormatVersion,
				Package.ReaderPolicyFingerprint);
			for (size_t StepIndex = 0; StepIndex < Package.Steps.size(); ++StepIndex)
			{
				if (StepIndex != 0) Json += ',';
				const auto& Step = Package.Steps[StepIndex];
				Json += std::format("{{\"handlerId\":\"{}\",\"kind\":\"{}\",\"sourceVersion\":{},\"targetVersion\":{},\"sourceCodecId\":\"{}\",\"targetCodecId\":\"{}\",\"strategy\":\"LoadTransformWrite\",\"risk\":\"{}\"}}",
					JsonEscape(Step.HandlerId), KindName(Step.Kind), Step.SourceVersion,
					Step.TargetVersion, JsonEscape(Step.SourceCodecId),
					JsonEscape(Step.TargetCodecId), RiskName(Step.Risk));
			}
			Json += "],\"diagnostics\":[";
			for (size_t DiagnosticIndex = 0; DiagnosticIndex < Package.Diagnostics.size(); ++DiagnosticIndex)
			{
				if (DiagnosticIndex != 0) Json += ',';
				Json += std::format("\"{}\"", JsonEscape(Package.Diagnostics[DiagnosticIndex]));
			}
			Json += "]}";
		}
		Json += std::format("],\"summary\":{{\"planned\":{},\"migrated\":0,\"skipped\":{},\"blocked\":{},\"failed\":{},\"rolledBack\":0}},\"changedPaths\":[]}}",
			Planned, Skipped, Blocked, Failed);
		return Json;
	}

	namespace
	{
		constexpr std::string_view MigrationPreSuffix = ".asset-migration-pre";
		constexpr std::string_view MigrationPostSuffix = ".asset-migration-post";
		constexpr std::string_view MigrationManifestSuffix = ".asset-migration-manifest";

		class FAssetMigrationWriterLock
		{
		public:
			auto Acquire(std::string& OutError) -> bool
			{
				Path = std::filesystem::path(FPaths::ProjectDir()) / "Saved" / "AssetMigrationWriter.lock";
				std::error_code DirectoryError;
				std::filesystem::create_directories(Path.parent_path(), DirectoryError);
				if (DirectoryError)
				{
					OutError = std::format("MigrationWriterLockFailed: {}", DirectoryError.message());
					return false;
				}
#if defined(_WIN32)
				Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
					OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
				if (Handle == INVALID_HANDLE_VALUE)
				{
					OutError = std::format("MigrationWriterBusy: another asset writer owns {}.", Path.generic_string());
					return false;
				}
#else
				File = open(Path.c_str(), O_CREAT | O_RDWR, 0600);
				if (File < 0 || flock(File, LOCK_EX | LOCK_NB) != 0)
				{
					if (File >= 0) close(File);
					File = -1;
					OutError = std::format("MigrationWriterBusy: another asset writer owns {}.", Path.generic_string());
					return false;
				}
#endif
				return true;
			}

			~FAssetMigrationWriterLock()
			{
#if defined(_WIN32)
				if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
#else
				if (File >= 0)
				{
					flock(File, LOCK_UN);
					close(File);
					std::error_code Error;
					std::filesystem::remove(Path, Error);
				}
#endif
			}

		private:
			std::filesystem::path Path;
#if defined(_WIN32)
			HANDLE Handle = INVALID_HANDLE_VALUE;
#else
			int File = -1;
#endif
		};

		struct FStagedMigrationPackage
		{
			size_t PlanIndex = 0;
			DPackage* Package = nullptr;
			std::filesystem::path Destination;
			std::filesystem::path PrePath;
			std::filesystem::path PostPath;
			std::filesystem::path ManifestPath;
			std::vector<uint8> PreBytes;
			std::vector<uint8> PostBytes;
			bool bPublished = false;
		};

		auto LoadMigrationBytes(const std::filesystem::path& Path, std::vector<uint8>& OutBytes) -> bool
		{
			return FFileHelper::LoadFileToArray(OutBytes, Path.generic_string());
		}

		auto SaveMigrationBytes(const std::filesystem::path& Path, std::span<const uint8> Bytes, std::string& OutError) -> bool
		{
			FFileHelper::FAtomicFileError Error;
			if (FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()}, Path, &Error)) return true;
			OutError = Error.ToString();
			return false;
		}

		auto IsCurrentFingerprint(
			const FAssetMigrationPackagePlan& Package,
			std::span<const uint8> Bytes) -> bool
		{
			std::error_code Error;
			const auto Time = std::filesystem::last_write_time(Package.PhysicalPath, Error);
			if (Error || Bytes.size() != Package.Fingerprint.FileSize) return false;
			return DerivedDataCache::FileTimeToStableTicks(Time) == Package.Fingerprint.LastWriteTimeTicks
				&& FXxHash128::HashBuffer(Bytes) == Package.Fingerprint.ContentHash;
		}

		auto IsCurrentPackageBytes(std::span<const uint8> Bytes) -> bool
		{
			if (Bytes.size() < sizeof(uint32) * 2) return false;
			uint32 Version = 0;
			std::memcpy(&Version, Bytes.data() + sizeof(uint32), sizeof(Version));
			return Version == AssetPackageMigrationWriterVersion;
		}

		auto SidecarPath(const std::filesystem::path& Destination, std::string_view Suffix)
			-> std::filesystem::path
		{
			return std::filesystem::path(Destination.generic_string() + std::string(Suffix));
		}

		auto CleanupMigrationSidecars(std::span<FStagedMigrationPackage> Entries) -> void
		{
			for (const FStagedMigrationPackage& Entry : Entries)
			{
				for (const auto& Path : {Entry.ManifestPath, Entry.PrePath, Entry.PostPath})
				{
					std::error_code Error;
					std::filesystem::remove(Path, Error);
				}
			}
		}

		auto WriteMigrationManifestState(
			std::span<FStagedMigrationPackage> Entries,
			std::string_view OperationId,
			std::string_view State,
			std::string& OutError) -> bool
		{
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				const FStagedMigrationPackage& Entry = Entries[Index];
				const std::string Manifest = std::format(
					"version=1\noperation={}\nstate={}\nindex={}\ndestination={}\n",
					OperationId, State, Index, Entry.Destination.generic_string());
				if (!SaveMigrationBytes(
						Entry.ManifestPath,
						std::span{reinterpret_cast<const uint8*>(Manifest.data()), Manifest.size()},
						OutError)) return false;
			}
			return true;
		}

		auto RecoverInterruptedAssetMigrationsLocked(std::string& OutError) -> bool
		{
			std::vector<std::filesystem::path> Manifests;
			std::vector<std::filesystem::path> Sidecars;
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				const std::filesystem::path Root = Mount.GetContentDir();
				std::error_code Error;
				if (!std::filesystem::exists(Root, Error) || Error) continue;
				std::filesystem::recursive_directory_iterator It(
					Root, std::filesystem::directory_options::skip_permission_denied, Error);
				for (const std::filesystem::recursive_directory_iterator End; !Error && It != End; It.increment(Error))
				{
					if (!It->is_regular_file()) continue;
					const std::string Path = It->path().generic_string();
					if (Path.ends_with(MigrationManifestSuffix)) Manifests.push_back(It->path());
					else if (Path.ends_with(MigrationPreSuffix) || Path.ends_with(MigrationPostSuffix)) Sidecars.push_back(It->path());
				}
				if (Error)
				{
					OutError = std::format("MigrationRecoveryScanFailed: {}", Error.message());
					return false;
				}
			}
			std::ranges::sort(Manifests);
			for (const std::filesystem::path& Manifest : Manifests)
			{
				std::string DestinationText = Manifest.generic_string();
				DestinationText.resize(DestinationText.size() - MigrationManifestSuffix.size());
				const std::filesystem::path Destination = DestinationText;
				const std::filesystem::path Pre = SidecarPath(Destination, MigrationPreSuffix);
				std::vector<uint8> Bytes;
				std::vector<uint8> Restored;
				if (!LoadMigrationBytes(Pre, Bytes) || !ValidateAssetPackageBytes(Bytes)
					|| !SaveMigrationBytes(Destination, Bytes, OutError)
					|| !LoadMigrationBytes(Destination, Restored) || Restored != Bytes)
				{
					OutError = std::format("MigrationRecoveryRequired: could not restore {}: {}",
						Destination.generic_string(), OutError);
					return false;
				}
			}
			for (const std::filesystem::path& Path : Manifests)
			{
				std::string Destination = Path.generic_string();
				Destination.resize(Destination.size() - MigrationManifestSuffix.size());
				for (const auto& Candidate : {Path, SidecarPath(Destination, MigrationPreSuffix), SidecarPath(Destination, MigrationPostSuffix)})
				{
					std::error_code Error;
					std::filesystem::remove(Candidate, Error);
				}
			}
			for (const std::filesystem::path& Path : Sidecars)
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
			return true;
		}

		auto SerializeMigrationReport(
			const FAssetMigrationPlan& Plan,
			std::string_view Operation,
			std::string_view ResultName,
			std::span<const std::string> ChangedPaths) -> std::string
		{
			uint64 Counts[6]{};
			for (const auto& Package : Plan.Packages) ++Counts[static_cast<size_t>(Package.Status)];
			std::string Json = std::format("{{\"schemaVersion\":{},\"operation\":\"{}\",\"result\":\"{}\",\"packages\":[",
				AssetMigrationReportSchemaVersion, Operation, ResultName);
			for (size_t Index = 0; Index < Plan.Packages.size(); ++Index)
			{
				if (Index) Json += ',';
				const auto& Package = Plan.Packages[Index];
				Json += std::format("{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"status\":\"{}\",\"fingerprint\":{{\"fileSize\":{},\"lastWriteTimeTicks\":{},\"contentHash\":\"{}\"}},\"sourceFormatVersion\":{},\"targetFormatVersion\":{},\"steps\":[",
					JsonEscape(Package.PackagePath.GetView()), JsonEscape(Package.PhysicalPath), StatusName(Package.Status),
					Package.Fingerprint.FileSize, Package.Fingerprint.LastWriteTimeTicks, JsonEscape(Package.ReportContentHash),
					Package.SourceFormatVersion, Package.TargetFormatVersion);
				for (size_t StepIndex = 0; StepIndex < Package.Steps.size(); ++StepIndex)
				{
					if (StepIndex) Json += ',';
					const auto& Step = Package.Steps[StepIndex];
					Json += std::format("{{\"handlerId\":\"{}\",\"kind\":\"{}\",\"sourceVersion\":{},\"targetVersion\":{},\"risk\":\"{}\"}}",
						JsonEscape(Step.HandlerId), KindName(Step.Kind), Step.SourceVersion, Step.TargetVersion, RiskName(Step.Risk));
				}
				Json += "],\"diagnostics\":[";
				for (size_t DiagnosticIndex = 0; DiagnosticIndex < Package.Diagnostics.size(); ++DiagnosticIndex)
				{
					if (DiagnosticIndex) Json += ',';
					Json += std::format("\"{}\"", JsonEscape(Package.Diagnostics[DiagnosticIndex]));
				}
				Json += "]}";
			}
			Json += std::format("],\"summary\":{{\"planned\":{},\"migrated\":{},\"skipped\":{},\"blocked\":{},\"failed\":{},\"rolledBack\":{}}},\"changedPaths\":[",
				Counts[0], Counts[1], Counts[2], Counts[3], Counts[4], Counts[5]);
			for (size_t Index = 0; Index < ChangedPaths.size(); ++Index)
			{
				if (Index) Json += ',';
				Json += std::format("\"{}\"", JsonEscape(ChangedPaths[Index]));
			}
			return Json + "]}";
		}
	}

	auto RecoverInterruptedAssetMigrations(std::string& OutError) -> bool
	{
		FAssetMigrationWriterLock Lock;
		return Lock.Acquire(OutError) && RecoverInterruptedAssetMigrationsLocked(OutError);
	}

	auto ApplyAssetPackageMigrations(
		FAssetMigrationPlan Plan,
		const FAssetMigrationRegistry& Registry,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetMigrationApplyOptions& Options,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetMigrationApplyResult
	{
		FAssetMigrationApplyResult Result{.Plan = std::move(Plan)};
		FAssetMigrationWriterLock Lock;
		if (!Lock.Acquire(Result.Diagnostic) || !RecoverInterruptedAssetMigrationsLocked(Result.Diagnostic))
		{
			Result.Status = EAssetMigrationApplyStatus::RecoveryRequired;
			for (auto& Package : Result.Plan.Packages)
				if (Package.Status == EAssetMigrationPackageStatus::Planned)
					Package.Status = EAssetMigrationPackageStatus::Failed;
			return Result;
		}
		if (Result.Plan.Status != EAssetMigrationPlanStatus::Completed
			|| std::ranges::any_of(Result.Plan.Packages, [](const auto& Package) {
				return Package.Status == EAssetMigrationPackageStatus::Blocked
					|| Package.Status == EAssetMigrationPackageStatus::Failed
					|| (Package.Status == EAssetMigrationPackageStatus::Planned
						&& Package.TargetFormatVersion != AssetPackageMigrationWriterVersion);
			}))
		{
			Result.Status = EAssetMigrationApplyStatus::Blocked;
			Result.Diagnostic = "MigrationApplyBlocked: the complete selected plan is not ready.";
			return Result;
		}

		const FAssetPackageLoadSnapshot LoadSnapshot = CapturePackageLoadSnapshot();
		std::vector<FStagedMigrationPackage> Entries;
		auto ReleaseLoaded = [&] { return ReleasePackagesLoadedSince(LoadSnapshot); };
		auto FailBeforePublish = [&](EAssetMigrationApplyStatus Status, std::string Diagnostic) {
			CleanupMigrationSidecars(Entries);
			(void)ReleaseLoaded();
			Result.Status = Status;
			Result.Diagnostic = std::move(Diagnostic);
			if (Status == EAssetMigrationApplyStatus::Failed)
				for (auto& Package : Result.Plan.Packages)
					if (Package.Status == EAssetMigrationPackageStatus::Planned)
					{
						Package.Status = EAssetMigrationPackageStatus::Failed;
						Package.Diagnostics.push_back(Result.Diagnostic);
					}
			return Result;
		};

		for (size_t Index = 0; Index < Result.Plan.Packages.size(); ++Index)
		{
			auto& PackagePlan = Result.Plan.Packages[Index];
			if (PackagePlan.Status != EAssetMigrationPackageStatus::Planned) continue;
			if (IsCancellationRequested && IsCancellationRequested())
				return FailBeforePublish(EAssetMigrationApplyStatus::Cancelled, "MigrationCancelled: apply was cancelled before publication.");
			if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::LoadPackage, Entries.size()))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed, "Injected migration package load failure.");

			FStagedMigrationPackage Entry{.PlanIndex = Index, .Destination = PackagePlan.PhysicalPath};
			if (!LoadMigrationBytes(Entry.Destination, Entry.PreBytes) || !IsCurrentFingerprint(PackagePlan, Entry.PreBytes))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
					std::format("StaleFingerprint: {} changed after planning.", PackagePlan.PackagePath.ToString()));
			if (PackagePlan.ReaderPolicyFingerprint != GetAssetPackageReaderPolicyIdentity())
				return FailBeforePublish(EAssetMigrationApplyStatus::Blocked,
					"MigrationAuthorizationChanged: the reader policy changed after planning.");
			const Private::FAssetPackageCodec* SourceCodec = nullptr;
			Private::FAssetPackagePreamble SourcePreamble;
			if (FAssetResult ResolveResult = Private::ResolveAssetPackageReader(
				Entry.PreBytes, SourceCodec, &SourcePreamble); !ResolveResult)
				return FailBeforePublish(EAssetMigrationApplyStatus::Blocked,
					std::format("MigrationSourceRejected: {}", ResolveResult.Message));
			if (SourcePreamble.FormatVersion != PackagePlan.SourceFormatVersion)
				return FailBeforePublish(EAssetMigrationApplyStatus::Blocked,
					"MigrationAuthorizationChanged: the source version no longer matches the plan.");
			auto Edge = Registry.ResolveExactEdge(
				EAssetMigrationKind::PackageFormat, SourcePreamble.FormatVersion,
				PackagePlan.TargetFormatVersion, IsCancellationRequested);
			if (Edge.Status != EAssetMigrationResolutionStatus::Resolved
				|| Edge.Steps.size() != 1 || PackagePlan.Steps.size() != 1
				|| !(Edge.Steps.front() == PackagePlan.Steps.front())
				|| Edge.Steps.front().Risk != EAssetMigrationRisk::Lossless
				|| Edge.Steps.front().SourceCodecId != SourceCodec->CodecId)
				return FailBeforePublish(EAssetMigrationApplyStatus::Blocked,
					"MigrationAuthorizationChanged: the exact lossless edge no longer matches the plan.");
			const Private::FAssetPackageCodec* TargetCodec =
				Private::FindAssetPackageWriter(PackagePlan.TargetFormatVersion);
			if (!TargetCodec || Edge.Steps.front().TargetCodecId != TargetCodec->CodecId)
				return FailBeforePublish(EAssetMigrationApplyStatus::Blocked,
					"MigrationTargetRejected: the exact edge does not name the selected writer codec.");
			FAssetLoadReport LoadReport;
			FAssetResult LoadResult = LoadPackageForMigration(PackagePlan.PackagePath, Entry.Package, &LoadReport);
			if (!LoadResult || !Entry.Package || LoadReport.HasRiskItems() || LoadReport.HasNonUpgradeMutations())
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
					std::format("MigrationLoadRejected: {}", LoadResult ? "unplanned compatibility or mutation risk" : LoadResult.Message));
			if (Edge.Steps.front().Transform)
			{
				FAssetResult TransformResult = Edge.Steps.front().Transform(
					Entry.Package, IsCancellationRequested);
				if (!TransformResult)
					return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
						std::format("MigrationTransformRejected: {}", TransformResult.Message));
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::SerializePackage, Entries.size()))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed, "Injected migration serialization failure.");
			FAssetResult SerializeResult = TargetCodec->Write(
				Entry.Package, Entry.PostBytes, EDefaultDeltaMode::NoDelta, {});
			std::vector<uint8> DeterminismBytes;
			if (!SerializeResult || !TargetCodec->Validate(Entry.PostBytes)
				|| !(SerializeResult = TargetCodec->Write(
					Entry.Package, DeterminismBytes, EDefaultDeltaMode::NoDelta, {}))
				|| Entry.PostBytes != DeterminismBytes || !IsCurrentPackageBytes(Entry.PostBytes))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
					std::format("MigrationSerializationRejected: {}", SerializeResult ? "current writer output was not deterministic" : SerializeResult.Message));
			Entry.PrePath = SidecarPath(Entry.Destination, MigrationPreSuffix);
			Entry.PostPath = SidecarPath(Entry.Destination, MigrationPostSuffix);
			Entry.ManifestPath = SidecarPath(Entry.Destination, MigrationManifestSuffix);
			Entries.push_back(std::move(Entry));
		}

		const std::string OperationId = std::format("{:016x}", static_cast<uint64>(
			std::chrono::steady_clock::now().time_since_epoch().count()));
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			if (IsCancellationRequested && IsCancellationRequested())
				return FailBeforePublish(EAssetMigrationApplyStatus::Cancelled, "MigrationCancelled: apply was cancelled during staging.");
			if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::StagePackage, Index))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed, "Injected migration staging failure.");
			auto& Entry = Entries[Index];
			std::string Error;
			if (!SaveMigrationBytes(Entry.PrePath, Entry.PreBytes, Error)
				|| !SaveMigrationBytes(Entry.PostPath, Entry.PostBytes, Error))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
					std::format("MigrationStagingFailed: {}", Error));
		}
		std::string ManifestError;
		if (!WriteMigrationManifestState(Entries, OperationId, "Staged", ManifestError))
			return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
				std::format("MigrationManifestFailed: {}", ManifestError));
		for (const auto& Entry : Entries)
		{
			std::vector<uint8> Current;
			if (!LoadMigrationBytes(Entry.Destination, Current)
				|| !IsCurrentFingerprint(Result.Plan.Packages[Entry.PlanIndex], Current))
				return FailBeforePublish(EAssetMigrationApplyStatus::Failed, "StaleFingerprint: selected inputs changed during preflight.");
		}

		auto Rollback = [&](std::string Diagnostic) {
			bool bRestored = true;
			for (size_t Count = Entries.size(); Count > 0; --Count)
			{
				auto& Entry = Entries[Count - 1];
				std::string Error;
				if ((Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::RollbackPackage, Count - 1))
					|| !SaveMigrationBytes(Entry.Destination, Entry.PreBytes, Error))
				{
					bRestored = false;
					continue;
				}
				std::vector<uint8> Restored;
				if (!LoadMigrationBytes(Entry.Destination, Restored) || Restored != Entry.PreBytes)
					bRestored = false;
			}
			(void)ReleaseLoaded();
			for (auto& Package : Result.Plan.Packages)
				if (Package.Status == EAssetMigrationPackageStatus::Planned)
				{
					Package.Status = bRestored ? EAssetMigrationPackageStatus::RolledBack : EAssetMigrationPackageStatus::Failed;
					Package.Diagnostics.push_back(Diagnostic);
				}
			if (bRestored) CleanupMigrationSidecars(Entries);
			Result.Status = bRestored ? EAssetMigrationApplyStatus::RolledBack : EAssetMigrationApplyStatus::RecoveryRequired;
			Result.Diagnostic = bRestored ? std::move(Diagnostic)
				: std::format("MigrationRecoveryRequired: rollback was incomplete after {}", Diagnostic);
			return Result;
		};

		if (!WriteMigrationManifestState(Entries, OperationId, "Publishing", ManifestError))
			return FailBeforePublish(EAssetMigrationApplyStatus::Failed,
				std::format("MigrationManifestFailed: {}", ManifestError));
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::PublishPackage, Index))
				return Rollback("Injected migration publication failure.");
			std::string Error;
			if (!SaveMigrationBytes(Entries[Index].Destination, Entries[Index].PostBytes, Error))
				return Rollback(std::format("MigrationPublicationFailed: {}", Error));
			Entries[Index].bPublished = true;
			if (!WriteMigrationManifestState(Entries, OperationId,
					std::format("Published:{}", Index + 1), ManifestError))
				return Rollback(std::format("MigrationManifestFailed: {}", ManifestError));
		}
		if (!WriteMigrationManifestState(Entries, OperationId, "Verifying", ManifestError))
			return Rollback(std::format("MigrationManifestFailed: {}", ManifestError));
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			std::vector<uint8> Published;
			if ((Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::VerifyPackage, Index))
				|| !LoadMigrationBytes(Entries[Index].Destination, Published)
				|| Published != Entries[Index].PostBytes || !IsCurrentPackageBytes(Published))
				return Rollback("MigrationVerificationFailed: published bytes did not match staged current output.");
		}
		if (FAssetResult ReleaseResult = ReleaseLoaded(); !ReleaseResult)
			return Rollback(std::format("MigrationUnloadFailed: {}", ReleaseResult.Message));

		if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::PostAudit, 0))
			return Rollback("Injected migration post-audit failure.");
		const FAssetPackageDiscoverySnapshot Snapshot = CaptureMountedAssetPackageSnapshot();
		if (Snapshot.Status != EAssetPackageSnapshotStatus::Completed)
			return Rollback("MigrationPostAuditFailed: fresh discovery did not complete.");
		std::vector<FAssetData> RegistryEntries;
		RegistryEntries.reserve(Entries.size());
		for (const FStagedMigrationPackage& Entry : Entries)
		{
			const FAssetPath& Path = Result.Plan.Packages[Entry.PlanIndex].PackagePath;
			const auto Input = std::ranges::find(Snapshot.Packages, Path, &FAssetPackageCompatibilityProbeInput::PackagePath);
			if (Input == Snapshot.Packages.end()) return Rollback("MigrationPostAuditFailed: migrated package disappeared.");
			auto Probe = ProbeAssetPackageCompatibility(*Input, Catalog);
			if (!Probe.Record || Probe.Record->FormatVersion != AssetPackageMigrationWriterVersion
				|| Probe.Record->Inspection != EAssetCompatibilityInspection::Ready
				|| Probe.Record->Compatibility != EAssetPackageCompatibility::Compatible
				|| !Probe.Record->Findings.empty())
				return Rollback("MigrationPostAuditFailed: migrated package is not clean and current.");
		}
		for (const FStagedMigrationPackage& Entry : Entries)
		{
			FAssetPackageHeader Header;
			FAssetResult HeaderResult = ReadAssetPackageHeader(Entry.Destination.generic_string(), Header);
			std::error_code Error;
			const auto LastWriteTime = std::filesystem::last_write_time(Entry.Destination, Error);
			const uintmax_t FileSize = Error ? 0 : std::filesystem::file_size(Entry.Destination, Error);
			if (!HeaderResult || Error || Header.FormatVersion != AssetPackageMigrationWriterVersion)
				return Rollback("MigrationRegistryPublicationFailed: migrated package metadata could not be published.");
			RegistryEntries.push_back({
				.PackagePath = Result.Plan.Packages[Entry.PlanIndex].PackagePath,
				.PhysicalPath = Entry.Destination.generic_string(),
				.AssetClassName = std::move(Header.AssetClassName),
				.EntryKind = Header.EntryKind,
				.RedirectDestination = std::move(Header.RedirectDestination),
				.FormatVersion = Header.FormatVersion,
				.Dependencies = std::move(Header.Dependencies),
				.FileSize = FileSize,
				.LastWriteTime = LastWriteTime,
				.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
		}
		if (Options.ShouldFail && Options.ShouldFail(EAssetMigrationApplyPhase::PublishRegistry, 0))
			return Rollback("Injected migration registry-publication failure.");
		FAssetManager::Get().PublishMigratedPackageRegistryEntries(RegistryEntries);

		for (const auto& Entry : Entries)
		{
			auto& Package = Result.Plan.Packages[Entry.PlanIndex];
			Package.Status = EAssetMigrationPackageStatus::Migrated;
			Result.ChangedPaths.push_back(Entry.Destination.generic_string());
		}
		CleanupMigrationSidecars(Entries);
		Result.Status = EAssetMigrationApplyStatus::Succeeded;
		return Result;
	}

	auto SerializeAssetMigrationApplyReportV2(const FAssetMigrationApplyResult& Result) -> std::string
	{
		std::string_view ResultName = "Failed";
		switch (Result.Status)
		{
		case EAssetMigrationApplyStatus::Succeeded: ResultName = "Succeeded"; break;
		case EAssetMigrationApplyStatus::Cancelled: ResultName = "Cancelled"; break;
		case EAssetMigrationApplyStatus::Blocked: ResultName = "Blocked"; break;
		case EAssetMigrationApplyStatus::RolledBack: ResultName = "RolledBack"; break;
		case EAssetMigrationApplyStatus::Failed:
		case EAssetMigrationApplyStatus::RecoveryRequired: break;
		}
		return SerializeMigrationReport(Result.Plan, "Apply", ResultName, Result.ChangedPaths);
	}
}

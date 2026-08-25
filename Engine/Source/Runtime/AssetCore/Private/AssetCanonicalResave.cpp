#include "Asset/CanonicalResave.h"

#include "Asset/PackageObjectStreamReader.h"
#include "AssetRuntimeStateInternal.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"

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
				case '\n': Result += "\\n"; break;
				case '\r': Result += "\\r"; break;
				case '\t': Result += "\\t"; break;
				default: Result += static_cast<char>(Character); break;
				}
			}
			return Result;
		}

		auto StatusName(EAssetCanonicalResavePackageStatus Status) -> std::string_view
		{
			switch (Status)
			{
			case EAssetCanonicalResavePackageStatus::Skipped: return "Skipped";
			case EAssetCanonicalResavePackageStatus::Ready: return "Ready";
			case EAssetCanonicalResavePackageStatus::Resaved: return "Resaved";
			case EAssetCanonicalResavePackageStatus::Blocked: return "Blocked";
			case EAssetCanonicalResavePackageStatus::Failed: return "Failed";
			case EAssetCanonicalResavePackageStatus::Cancelled: return "Cancelled";
			case EAssetCanonicalResavePackageStatus::Stale: return "Stale";
			}
			return "Failed";
		}

		auto KindName(EAssetReflectedIdentityKind Kind) -> std::string_view
		{
			switch (Kind)
			{
			case EAssetReflectedIdentityKind::Class: return "Class";
			case EAssetReflectedIdentityKind::Struct: return "Struct";
			case EAssetReflectedIdentityKind::Enum: return "Enum";
			case EAssetReflectedIdentityKind::Property: return "Property";
			}
			return "Class";
		}

		auto LocationName(EAssetSerializedIdentityLocation Location) -> std::string_view
		{
			switch (Location)
			{
			case EAssetSerializedIdentityLocation::PackageHeader: return "PackageHeader";
			case EAssetSerializedIdentityLocation::ObjectRecord: return "ObjectRecord";
			case EAssetSerializedIdentityLocation::Schema: return "Schema";
			case EAssetSerializedIdentityLocation::TypeDescriptor: return "TypeDescriptor";
			}
			return "PackageHeader";
		}

		auto IsSelected(const FAssetPath& Path, const FAssetCanonicalResaveSelection& Selection) -> bool
		{
			if (Selection.bWholeProject || (Selection.Mounts.empty() && Selection.Folders.empty()
				&& Selection.Packages.empty())) return true;
			if (std::ranges::find(Selection.Packages, Path) != Selection.Packages.end()) return true;
			if (std::ranges::any_of(Selection.Folders, [&](const std::string& Folder) {
				const std::string_view Value = Path.GetView();
				return Value.starts_with(Folder) && Value.size() > Folder.size()
					&& Value[Folder.size()] == '/';
			})) return true;
			return std::ranges::any_of(Selection.Mounts, [&](const std::string& Mount) {
				const std::string_view Value = Path.GetView();
				return Value.starts_with(Mount) && Value.size() > Mount.size() && Value[Mount.size()] == '/';
			});
		}

		auto LoadBytes(std::string_view Path, std::vector<std::byte>& OutBytes) -> bool
		{
			return FFileHelper::LoadFileToArray(OutBytes, Path);
		}

		auto FingerprintMatches(const FAssetPackageFingerprint& Fingerprint,
			std::span<const std::byte> Bytes, std::string_view PhysicalPath) -> bool
		{
			std::error_code Error;
			const auto Time = std::filesystem::last_write_time(PhysicalPath, Error);
			return !Error && Fingerprint.FileSize == Bytes.size()
				&& Fingerprint.LastWriteTimeTicks == FileTime::ToStableTicks(Time)
				&& Fingerprint.ContentHash == FXxHash128::HashBuffer(Bytes);
		}

		auto SerializePackages(const FAssetCanonicalResavePlan& Plan) -> std::string
		{
			std::string Json = "[";
			for (size_t Index = 0; Index < Plan.Packages.size(); ++Index)
			{
				if (Index) Json += ',';
				const auto& Package = Plan.Packages[Index];
				Json += std::format("{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"status\":\"{}\",\"formatVersion\":{},\"loaded\":{},\"dirty\":{},\"plainResave\":{},\"evidence\":[",
					JsonEscape(Package.PackagePath.GetView()), JsonEscape(Package.PhysicalPath),
					StatusName(Package.Status), Package.FormatVersion, Package.bLoaded,
					Package.bDirty, Package.bPlainResaveRequested);
				for (size_t EvidenceIndex = 0; EvidenceIndex < Package.Evidence.size(); ++EvidenceIndex)
				{
					if (EvidenceIndex) Json += ',';
					const auto& Evidence = Package.Evidence[EvidenceIndex];
					Json += std::format("{{\"storedIdentity\":\"{}\",\"currentIdentity\":\"{}\",\"kind\":\"{}\",\"location\":\"{}\",\"logicalPath\":\"{}\"}}",
						JsonEscape(Evidence.StoredIdentity), JsonEscape(Evidence.CurrentIdentity),
						KindName(Evidence.Kind), LocationName(Evidence.Location), JsonEscape(Evidence.LogicalPath));
				}
				Json += "],\"deprecatedRouteEvidence\":[";
				for (size_t EvidenceIndex = 0;
					EvidenceIndex < Package.DeprecatedRouteEvidence.size(); ++EvidenceIndex)
				{
					if (EvidenceIndex) Json += ',';
					const auto& Evidence = Package.DeprecatedRouteEvidence[EvidenceIndex];
					Json += std::format(
						"{{\"objectPath\":\"{}\",\"declaringType\":\"{}\",\"storedFieldName\":\"{}\",\"deprecatedPropertyName\":\"{}\",\"customVersionGuid\":\"{}\",\"sourceVersion\":{},\"deprecatedBefore\":{}}}",
						JsonEscape(Evidence.ObjectPath), JsonEscape(Evidence.DeclaringType),
						JsonEscape(Evidence.StoredFieldName),
						JsonEscape(Evidence.DeprecatedPropertyName),
						Evidence.CustomVersionGuid.ToString(), Evidence.SourceVersion,
						Evidence.DeprecatedBefore);
				}
				Json += "],\"diagnostics\":[";
				for (size_t DiagnosticIndex = 0; DiagnosticIndex < Package.Diagnostics.size(); ++DiagnosticIndex)
				{
					if (DiagnosticIndex) Json += ',';
					Json += std::format("\"{}\"", JsonEscape(Package.Diagnostics[DiagnosticIndex]));
				}
				Json += "]}";
			}
			return Json + "]";
		}
	}

	auto PlanAssetCanonicalResaves(std::span<const FAssetPackageCompatibilityRecord> Records,
		const FAssetCanonicalResaveSelection& Selection,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetCanonicalResavePlan
	{
		FAssetCanonicalResavePlan Plan;
		Plan.RegistryRevision = GetAssetCatalogStore().GetRevision();
		Plan.TargetFormatVersion = AssetPackageV6FormatVersion;
		std::vector<const FAssetPackageCompatibilityRecord*> Sorted;
		for (const auto& Record : Records)
			if (IsSelected(Record.PackagePath, Selection)) Sorted.push_back(&Record);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		for (const auto* Record : Sorted)
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Plan.Status = EAssetCanonicalResavePlanStatus::Cancelled;
				break;
			}
			DPackage* Loaded = FAssetRuntimeState::Get().GetLoadService().FindResidentPackage(Record->PackagePath);
			FAssetCanonicalResavePackagePlan& Package = Plan.Packages.emplace_back();
			Package.PackagePath = Record->PackagePath;
			Package.PhysicalPath = Record->PhysicalPath;
			Package.Fingerprint = Record->Fingerprint;
			Package.FormatVersion = Record->FormatVersion;
			Package.EntryKind = Record->EntryKind;
			Package.bLoaded = Loaded != nullptr;
			Package.bDirty = Loaded && Loaded->IsDirty();
			Package.bPlainResaveRequested = Record->EntryKind
				== EAssetRegistryEntryKind::Asset
				&& (Record->FormatVersion != Plan.TargetFormatVersion
					|| (Selection.bAllowPlainResave
						&& std::ranges::find(Selection.Packages, Record->PackagePath)
							!= Selection.Packages.end()));
			Package.Evidence = Record->CanonicalizationEvidence;
			Package.DeprecatedRouteEvidence = Record->DeprecatedRouteEvidence;
			if (Record->Inspection != EAssetCompatibilityInspection::Ready
				|| Record->Compatibility != EAssetPackageCompatibility::Compatible)
				Package.Diagnostics.push_back("CompatibilityBlocked: package inspection is not compatible and ready.");
			if (Record->Freshness != EAssetCompatibilityFreshness::Current)
				Package.Diagnostics.push_back("StaleFingerprint: package changed during inspection.");
			if (Record->FormatVersion != AssetPackageV6FormatVersion)
				Package.Diagnostics.push_back(
					"NonCurrentFormat: canonical resave accepts only DAST v6 packages.");
			const PathUtilities::FMountLookupResult Mount =
				PathUtilities::FindMountForVirtualPath(Record->PackagePath.GetView());
			if (!Mount || !Mount.Mount->bAuthoringWritable)
				Package.Diagnostics.push_back("ReadOnlyMount: package is not on an authoring-writable mount.");
			if (Package.bDirty)
				Package.Diagnostics.push_back("DirtyConflict: loaded package has authored changes.");
			if (!Package.Diagnostics.empty()) Package.Status = EAssetCanonicalResavePackageStatus::Blocked;
			else if (Package.Evidence.empty() && Package.DeprecatedRouteEvidence.empty()
				&& !Package.bPlainResaveRequested)
				Package.Status = EAssetCanonicalResavePackageStatus::Skipped;
			else Package.Status = EAssetCanonicalResavePackageStatus::Ready;
		}
		return Plan;
	}

	auto ApplyAssetCanonicalResaves(FAssetCanonicalResavePlan Plan,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCanonicalResaveApplyOptions& Options,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetCanonicalResaveApplyResult
	{
		FAssetCanonicalResaveApplyResult Result{.Plan = std::move(Plan)};
		if (Result.Plan.Status != EAssetCanonicalResavePlanStatus::Completed)
		{
			Result.Status = EAssetCanonicalResaveApplyStatus::Cancelled;
			Result.Diagnostic = "CanonicalResaveCancelled: planning did not complete.";
			return Result;
		}
		if (Result.Plan.RegistryRevision != GetAssetCatalogStore().GetRevision())
		{
			Result.Status = EAssetCanonicalResaveApplyStatus::Blocked;
			Result.Diagnostic = "CanonicalResaveRegistryStale: registry changed after planning.";
			return Result;
		}
		if (Options.MaximumPackagesPerBatch == 0
			|| Options.MaximumPackagesPerBatch > MaximumCanonicalResaveBatchPackages)
		{
			Result.Status = EAssetCanonicalResaveApplyStatus::Blocked;
			Result.Diagnostic = "CanonicalResaveBatchLimitInvalid: requested batch size is outside the supported bound.";
			return Result;
		}
		if (std::ranges::any_of(Result.Plan.Packages, [](const auto& Package) {
			return Package.Status == EAssetCanonicalResavePackageStatus::Blocked; }))
		{
			Result.Status = EAssetCanonicalResaveApplyStatus::Blocked;
			Result.Diagnostic = "CanonicalResaveApplyBlocked: selected plan contains blockers.";
			return Result;
		}

		size_t Completed = 0;
		for (size_t Index = 0; Index < Result.Plan.Packages.size(); ++Index)
		{
			auto& PackagePlan = Result.Plan.Packages[Index];
			if (PackagePlan.Status != EAssetCanonicalResavePackageStatus::Ready) continue;
			if (IsCancellationRequested && IsCancellationRequested())
			{
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Cancelled;
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial
					: EAssetCanonicalResaveApplyStatus::Cancelled;
				Result.Diagnostic = "CanonicalResaveCancelled: admission stopped before the next package.";
				return Result;
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetCanonicalResaveApplyPhase::Revalidate, Index))
			{
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Failed;
				Result.Diagnostic = "Injected canonical-resave revalidation failure.";
				return Result;
			}
			std::vector<std::byte> BeforeBytes;
			if (!LoadBytes(PackagePlan.PhysicalPath, BeforeBytes)
				|| !FingerprintMatches(PackagePlan.Fingerprint, BeforeBytes, PackagePlan.PhysicalPath))
			{
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Stale;
				PackagePlan.Diagnostics.push_back("StaleFingerprint: package changed after planning.");
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial
					: EAssetCanonicalResaveApplyStatus::Failed;
				Result.Diagnostic = PackagePlan.Diagnostics.back();
				return Result;
			}

			const FAssetPackageLoadSnapshot Snapshot = CapturePackageLoadSnapshot();
			DPackage* Package = FAssetRuntimeState::Get().GetLoadService().FindResidentPackage(PackagePlan.PackagePath);
			const bool bWasLoaded = Package != nullptr;
			FAssetLoadReport LoadReport;
			if (!Package)
			{
				if (Options.ShouldFail && Options.ShouldFail(EAssetCanonicalResaveApplyPhase::LoadPackage, Index))
				{
					PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
					Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Failed;
					Result.Diagnostic = "Injected canonical-resave load failure.";
					return Result;
				}
				DObject* Asset = nullptr;
				FAssetResult Load = LoadAsset(
					PackagePlan.PackagePath, Asset, &LoadReport);
				Package = Load && Asset ? Asset->GetPackage() : nullptr;
				if (!Load || !Package || LoadReport.HasNonUpgradeMutations())
				{
					(void)ReleasePackagesLoadedSince(Snapshot);
					PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
					Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Failed;
					Result.Diagnostic = std::format("CanonicalResaveLoadRejected: {}",
						Load ? "load reported compatibility or mutation risk" : Load.Message);
					return Result;
				}
			}
			if (Options.PrepareLoadedAsset)
			{
				const FAssetResult Prepared = Options.PrepareLoadedAsset(
					PackagePlan.PackagePath, Package->GetAsset());
				if (!Prepared)
				{
					if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
					PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
					PackagePlan.Diagnostics.push_back(std::format(
						"CanonicalResavePrepareRejected: {}", Prepared.Message));
					Result.Status = Completed
						? EAssetCanonicalResaveApplyStatus::Partial
						: EAssetCanonicalResaveApplyStatus::Failed;
					Result.Diagnostic = PackagePlan.Diagnostics.back();
					return Result;
				}
			}
			if (Package->IsDirty())
			{
				if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Blocked;
				PackagePlan.Diagnostics.push_back("DirtyConflict: package became dirty after planning.");
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Blocked;
				Result.Diagnostic = PackagePlan.Diagnostics.back();
				return Result;
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetCanonicalResaveApplyPhase::SerializePackage, Index))
			{
				if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial
					: EAssetCanonicalResaveApplyStatus::Failed;
				Result.Diagnostic = "Injected canonical-resave serialization failure.";
				return Result;
			}
			DPackage* Unit[] = {Package};
			FAssetBundleSaveOptions SaveOptions;
			SaveOptions.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				if (!Options.ShouldFail) return false;
				if (Phase == EAssetBundleSavePhase::StagePackage)
					return Options.ShouldFail(EAssetCanonicalResaveApplyPhase::StagePackage, Index);
				if (Phase == EAssetBundleSavePhase::PublishPackage
					|| Phase == EAssetBundleSavePhase::PublishRootPackage)
					return Options.ShouldFail(EAssetCanonicalResaveApplyPhase::PublishPackage, Index);
				if (Phase == EAssetBundleSavePhase::PublishRegistry)
					return Options.ShouldFail(EAssetCanonicalResaveApplyPhase::PublishRegistry, Index);
				return false;
			};
			FAssetResult Save = SavePackagesAtomically(Unit, SaveOptions);
			if (!Save)
			{
				if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
				PackagePlan.Diagnostics.push_back(Save.Message);
				Result.Status = Completed ? EAssetCanonicalResaveApplyStatus::Partial
					: EAssetCanonicalResaveApplyStatus::Failed;
				Result.Diagnostic = Save.Message;
				return Result;
			}

			std::vector<std::byte> AfterBytes;
			FAssetPackageCompatibilityRecord Verification;
			const bool bInjectedVerificationFailure = Options.ShouldFail
				&& Options.ShouldFail(EAssetCanonicalResaveApplyPhase::VerifyPackage, Index);
			FAssetResult Verify = {EAssetError::IoError,
				"Published package could not be reread."};
			if (LoadBytes(PackagePlan.PhysicalPath, AfterBytes))
			{
				FAssetPackageInspection Inspection;
				Verify = InspectAssetPackage(PackagePlan.PhysicalPath, Inspection);
				Verification.FormatVersion = Inspection.Header.FormatVersion;
				Verification.Compatibility = Verify
					? EAssetPackageCompatibility::Compatible
					: EAssetPackageCompatibility::Incompatible;
			}
			if (bInjectedVerificationFailure || !Verify
				|| Verification.FormatVersion != Result.Plan.TargetFormatVersion
				|| Verification.Compatibility != EAssetPackageCompatibility::Compatible
				|| !Verification.CanonicalizationEvidence.empty()
				|| !Verification.DeprecatedRouteEvidence.empty())
			{
				FFileHelper::FAtomicFileError RestoreError;
				const bool bRestored = FFileHelper::SaveArrayToFileAtomically(
					std::as_bytes(std::span(BeforeBytes)), PackagePlan.PhysicalPath, &RestoreError);
				Package->SetCanonicalResaveRecommended(true);
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
				PackagePlan.Diagnostics.push_back(bRestored
					? "CanonicalResaveVerificationFailed: prior package bytes were restored."
					: std::format("CanonicalResaveRecoveryRequired: verification failed and prior bytes could not be restored: {}",
						RestoreError.ToString()));
				if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
				Result.Status = bRestored
					? (Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Failed)
					: EAssetCanonicalResaveApplyStatus::RecoveryRequired;
				Result.Diagnostic = PackagePlan.Diagnostics.back();
				return Result;
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetCanonicalResaveApplyPhase::ReconcileRegistry, Index))
			{
				FFileHelper::FAtomicFileError RestoreError;
				const bool bRestored = FFileHelper::SaveArrayToFileAtomically(
					std::as_bytes(std::span(BeforeBytes)), PackagePlan.PhysicalPath, &RestoreError);
				Package->SetCanonicalResaveRecommended(true);
				if (!bWasLoaded) (void)ReleasePackagesLoadedSince(Snapshot);
				PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
				PackagePlan.Diagnostics.push_back(bRestored
					? "CanonicalResaveRegistryReconciliationFailed: prior package bytes were restored."
					: std::format("CanonicalResaveRecoveryRequired: registry reconciliation failed and restore failed: {}",
						RestoreError.ToString()));
				Result.Status = bRestored
					? (Completed ? EAssetCanonicalResaveApplyStatus::Partial : EAssetCanonicalResaveApplyStatus::Failed)
					: EAssetCanonicalResaveApplyStatus::RecoveryRequired;
				Result.Diagnostic = PackagePlan.Diagnostics.back();
				return Result;
			}
			Package->SetCanonicalResaveRecommended(false);
			if (!bWasLoaded)
			{
				FAssetResult Release = ReleasePackagesLoadedSince(Snapshot);
				if (!Release)
				{
					PackagePlan.Status = EAssetCanonicalResavePackageStatus::Failed;
					Result.Status = EAssetCanonicalResaveApplyStatus::Partial;
					Result.Diagnostic = Release.Message;
					return Result;
				}
			}
			PackagePlan.Status = EAssetCanonicalResavePackageStatus::Resaved;
			Result.ChangedPaths.push_back(PackagePlan.PhysicalPath);
			++Completed;
		}
		Result.Status = EAssetCanonicalResaveApplyStatus::Succeeded;
		return Result;
	}

	auto SerializeAssetCanonicalResavePlanReport(const FAssetCanonicalResavePlan& Plan) -> std::string
	{
		return std::format("{{\"schemaVersion\":{},\"operation\":\"canonical-resave\",\"mode\":\"plan\",\"status\":\"{}\",\"registryRevision\":{},\"targetFormatVersion\":{},\"packages\":{}}}",
			AssetCanonicalResaveReportSchemaVersion,
			Plan.Status == EAssetCanonicalResavePlanStatus::Completed ? "Completed" : "Cancelled",
			Plan.RegistryRevision, Plan.TargetFormatVersion, SerializePackages(Plan));
	}

	auto SerializeAssetCanonicalResaveApplyReport(const FAssetCanonicalResaveApplyResult& Result) -> std::string
	{
		auto ApplyStatusName = [](EAssetCanonicalResaveApplyStatus Status) -> std::string_view {
			switch (Status)
			{
			case EAssetCanonicalResaveApplyStatus::Succeeded: return "Succeeded";
			case EAssetCanonicalResaveApplyStatus::Partial: return "Partial";
			case EAssetCanonicalResaveApplyStatus::Cancelled: return "Cancelled";
			case EAssetCanonicalResaveApplyStatus::Blocked: return "Blocked";
			case EAssetCanonicalResaveApplyStatus::Failed: return "Failed";
			case EAssetCanonicalResaveApplyStatus::RecoveryRequired: return "RecoveryRequired";
			}
			return "Failed";
		};
		return std::format("{{\"schemaVersion\":{},\"operation\":\"canonical-resave\",\"mode\":\"apply\",\"status\":\"{}\",\"diagnostic\":\"{}\",\"targetFormatVersion\":{},\"packages\":{}}}",
			AssetCanonicalResaveReportSchemaVersion, ApplyStatusName(Result.Status),
			JsonEscape(Result.Diagnostic), Result.Plan.TargetFormatVersion,
			SerializePackages(Result.Plan));
	}
}

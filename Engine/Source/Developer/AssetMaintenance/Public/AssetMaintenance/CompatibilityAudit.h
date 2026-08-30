#pragma once

#include "AssetMaintenanceAPI.h"
#include "Asset/PackageSchema.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetCompatibilityReportSchemaVersion = 3;

	enum class EAssetCompatibilityFindingCode : uint8
	{
		UnknownField,
		IncompatibleFieldSignature,
		DeprecatedRouteUsed,
		UnavailableClass,
		UnsupportedPackageFormat,
		InvalidObjectGraph,
		CorruptPackage,
		IoFailure
	};
	enum class EAssetCompatibilityInspection : uint8 { NotChecked, Ready, Failed };
	enum class EAssetPackageCompatibility : uint8 { Compatible, Incompatible, Unsupported };
	enum class EAssetCompatibilityFreshness : uint8 { Current, Stale };
	enum class EAssetCompatibilityProbeStatus : uint8 { Completed, Cancelled };

	using FReflectionCompatibilityCatalog = FReflectionSchemaCatalog;

	struct FAssetCompatibilityFinding
	{
		EAssetCompatibilityFindingCode Code = EAssetCompatibilityFindingCode::CorruptPackage;
		std::string ObjectPath;
		std::string ClassIdentity;
		std::string DeclaringType;
		std::string FieldName;
		DurinCodeGen::EPropertyGenFlags StoredKind = DurinCodeGen::EPropertyGenFlags::None;
		std::string StoredTypeSignature;
		DurinCodeGen::EPropertyGenFlags ExpectedKind = DurinCodeGen::EPropertyGenFlags::None;
		std::string ExpectedTypeSignature;
		uint64 PayloadSize = 0;
		uint64 PayloadOffset = 0;
		std::string Diagnostic;
		auto operator==(const FAssetCompatibilityFinding&) const -> bool = default;
	};

	struct FAssetPackageCompatibilityRecord
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		FAssetPackageFingerprint Fingerprint;
		std::string ReportContentHash;
		uint32 FormatVersion = 0;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::vector<FAssetPath> Dependencies;
		EAssetCompatibilityInspection Inspection = EAssetCompatibilityInspection::NotChecked;
		EAssetPackageCompatibility Compatibility = EAssetPackageCompatibility::Unsupported;
		EAssetCompatibilityFreshness Freshness = EAssetCompatibilityFreshness::Current;
		std::vector<FAssetCompatibilityFinding> Findings;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;
		auto operator==(const FAssetPackageCompatibilityRecord&) const -> bool = default;
	};

	using FAssetCompatibilityProbeStats = FPackageSchemaReadStats;

	struct FAssetPackageCompatibilityProbeInput
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		uintmax_t ExpectedFileSize = 0;
		int64 ExpectedLastWriteTimeTicks = 0;
		FXxHash128 ExpectedContentHash;
		std::string ExpectedReportContentHash;
		bool bIncludeNestedMigrationEvidence = false;
	};

	struct FAssetPackageCompatibilityProbeResult
	{
		EAssetCompatibilityProbeStatus Status = EAssetCompatibilityProbeStatus::Completed;
		std::optional<FAssetPackageCompatibilityRecord> Record;
		FAssetCompatibilityProbeStats Stats;
	};

	using FAssetCompatibilityCancellationCheck = FPackageReadCancellationCheck;

	enum class EAssetPackageSnapshotStatus : uint8 { Completed, Cancelled, Failed };

	struct FAssetPackageDiscoverySnapshot
	{
		EAssetPackageSnapshotStatus Status = EAssetPackageSnapshotStatus::Completed;
		std::vector<FAssetPackageCompatibilityProbeInput> Packages;
		std::string Error;
	};

	enum class EAssetCompatibilityAuditStatus : uint8 { Completed, Cancelled };

	struct FAssetCompatibilityAuditResult
	{
		EAssetCompatibilityAuditStatus Status = EAssetCompatibilityAuditStatus::Completed;
		std::vector<FAssetPackageCompatibilityRecord> Records;
	};

	using FAssetCompatibilityRecordSink = std::function<void(
		const FAssetPackageCompatibilityRecord&, uint64 Completed, uint64 Total)>;
	using FAssetCompatibilityProbeOperation = std::function<FAssetPackageCompatibilityProbeResult(
		const FAssetPackageCompatibilityProbeInput&, const FReflectionCompatibilityCatalog&,
		const FAssetCompatibilityCancellationCheck&)>;

	// Captures authoring auto-scan mounts without constructing or publishing assets.
	// Strong content hashes are retained for reproducible CLI reports and resave admission.
	ASSETMAINTENANCE_API auto CaptureMountedAssetPackageSnapshot(
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageDiscoverySnapshot;

	ASSETMAINTENANCE_API auto ProbeAssetPackageCompatibility(
		const FAssetPackageCompatibilityProbeInput& Input,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageCompatibilityProbeResult;

	ASSETMAINTENANCE_API auto IsAssetPackageCompatibilityRecordCurrent(
		const FAssetPackageCompatibilityRecord& Record, uintmax_t FileSize,
		int64 LastWriteTimeTicks) -> bool;

	ASSETMAINTENANCE_API auto AssetCompatibilityFindingCodeName(
		EAssetCompatibilityFindingCode Code) -> std::string_view;

	// UI-neutral deterministic batch operation shared by editor tasks and command-line hosts.
	ASSETMAINTENANCE_API auto RunAssetCompatibilityAudit(
		std::span<const FAssetPackageCompatibilityProbeInput> Inputs,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {},
		const FAssetCompatibilityRecordSink& OnRecord = {},
		const FAssetCompatibilityProbeOperation& Probe = {})
		-> FAssetCompatibilityAuditResult;

	ASSETMAINTENANCE_API auto SerializeAssetCompatibilityReport(
		std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string;
}

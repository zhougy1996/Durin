#pragma once

#include "AssetMaintenanceAPI.h"
#include "Asset/Compatibility.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetCompatibilityReportSchemaVersion = 3;

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

	// UI-neutral deterministic batch operation shared by editor tasks and command-line hosts.
	ASSETMAINTENANCE_API auto RunAssetCompatibilityAudit(
		std::span<const FAssetPackageCompatibilityProbeInput> Inputs,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {},
		const FAssetCompatibilityRecordSink& OnRecord = {},
		const FAssetCompatibilityProbeOperation& Probe = {})
		-> FAssetCompatibilityAuditResult;

	ASSETMAINTENANCE_API auto SerializeAssetCompatibilityReportV1(
		std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string;
}

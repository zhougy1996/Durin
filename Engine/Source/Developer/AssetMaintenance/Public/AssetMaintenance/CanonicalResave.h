#pragma once

#include "AssetMaintenanceAPI.h"
#include "AssetMaintenance/CompatibilityAudit.h"

namespace Durin
{
	inline constexpr uint32 AssetCanonicalResaveReportSchemaVersion = 3;
	inline constexpr size_t MaximumCanonicalResaveBatchPackages = 32;

	enum class EAssetCanonicalResavePackageStatus : uint8
	{
		Skipped,
		Ready,
		Resaved,
		Blocked,
		Failed,
		Cancelled,
		Stale
	};

	// Storage facts observed from decoded source and its detached recompression candidate.
	struct FTextureSourceRecompressionRecord
	{
		std::string ObjectPath;
		std::string Identity;
		std::string DecodedHash;
		std::string BulkInstance;
		std::string Descriptor;
		uint64 DecodedBytes = 0;
		uint64 StoredBytesBefore = 0;
		uint64 StoredBytesAfter = 0;
		bool bChanged = false;
	};

	struct FAssetCanonicalResavePackagePlan
	{
		FPackagePath PackagePath;
		std::string PhysicalPath;
		FAssetPackageFingerprint Fingerprint;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		EAssetCanonicalResavePackageStatus Status = EAssetCanonicalResavePackageStatus::Blocked;
		bool bLoaded = false;
		bool bDirty = false;
		bool bPlainResaveRequested = false;
		bool bRecompressTextureSources = false;
		std::vector<FTextureSourceRecompressionRecord> TextureSources;
		std::vector<FAssetCanonicalizationEvidence> Evidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;
		std::vector<std::string> Diagnostics;
	};

	struct FAssetCanonicalResaveSelection
	{
		std::vector<std::string> Mounts;
		std::vector<std::string> Folders;
		std::vector<FPackagePath> Packages;
		bool bWholeProject = false;
		bool bAllowPlainResave = false;
		bool bRecompressTextureSources = false;
	};

	enum class EAssetCanonicalResavePlanStatus : uint8 { Completed, Cancelled };
	struct FAssetCanonicalResavePlan
	{
		EAssetCanonicalResavePlanStatus Status = EAssetCanonicalResavePlanStatus::Completed;
		uint64 RegistryRevision = 0;
		std::vector<FAssetCanonicalResavePackagePlan> Packages;
	};

	enum class EAssetCanonicalResaveApplyStatus : uint8
	{
		Succeeded,
		Partial,
		Cancelled,
		Blocked,
		Failed,
		RecoveryRequired
	};

	enum class EAssetCanonicalResaveApplyPhase : uint8
	{
		Revalidate,
		LoadPackage,
		SerializePackage,
		StagePackage,
		PublishPackage,
		PublishRegistry,
		VerifyPackage,
		ReconcileRegistry
	};

	struct FAssetCanonicalResaveApplyOptions
	{
		// Preview loads sources and compresses detached candidates without publishing them.
		bool bPreview = false;
		size_t MaximumPackagesPerBatch = MaximumCanonicalResaveBatchPackages;
		std::function<bool(EAssetCanonicalResaveApplyPhase, size_t)> ShouldFail;
		// Tool hosts may wait for editor-only post-load recovery and reject an
		// asset that has not reached its domain-ready state before serialization.
		std::function<FAssetResult(const FPackagePath&, DObject*)> PrepareLoadedAsset;
	};

	struct FAssetCanonicalResaveApplyResult
	{
		EAssetCanonicalResaveApplyStatus Status = EAssetCanonicalResaveApplyStatus::Failed;
		FAssetCanonicalResavePlan Plan;
		std::vector<std::string> ChangedPaths;
		std::string Diagnostic;
	};

	ASSETMAINTENANCE_API auto PlanAssetCanonicalResaves(
		std::span<const FAssetPackageCompatibilityRecord> Records,
		const FAssetCanonicalResaveSelection& Selection = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetCanonicalResavePlan;

	ASSETMAINTENANCE_API auto ApplyAssetCanonicalResaves(
		FAssetCanonicalResavePlan Plan,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCanonicalResaveApplyOptions& Options = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetCanonicalResaveApplyResult;

	ASSETMAINTENANCE_API auto SerializeAssetCanonicalResavePlanReport(
		const FAssetCanonicalResavePlan& Plan) -> std::string;
	ASSETMAINTENANCE_API auto SerializeAssetCanonicalResaveApplyReport(
		const FAssetCanonicalResaveApplyResult& Result) -> std::string;
}

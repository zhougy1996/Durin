#pragma once

#include "Asset/Compatibility.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetCanonicalResaveReportSchemaVersion = 1;
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

	struct FAssetCanonicalResavePackagePlan
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		FAssetPackageFingerprint Fingerprint;
		uint32 FormatVersion = 0;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		EAssetCanonicalResavePackageStatus Status = EAssetCanonicalResavePackageStatus::Blocked;
		bool bLoaded = false;
		bool bDirty = false;
		bool bPlainResaveRequested = false;
		std::vector<FAssetCanonicalizationEvidence> Evidence;
		std::vector<std::string> Diagnostics;
	};

	struct FAssetCanonicalResaveSelection
	{
		std::vector<std::string> Mounts;
		std::vector<std::string> Folders;
		std::vector<FAssetPath> Packages;
		bool bWholeProject = false;
		bool bAllowPlainResave = false;
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
		size_t MaximumPackagesPerBatch = MaximumCanonicalResaveBatchPackages;
		std::function<bool(EAssetCanonicalResaveApplyPhase, size_t)> ShouldFail;
	};

	struct FAssetCanonicalResaveApplyResult
	{
		EAssetCanonicalResaveApplyStatus Status = EAssetCanonicalResaveApplyStatus::Failed;
		FAssetCanonicalResavePlan Plan;
		std::vector<std::string> ChangedPaths;
		std::string Diagnostic;
	};

	ASSETCORE_API auto PlanAssetCanonicalResaves(
		std::span<const FAssetPackageCompatibilityRecord> Records,
		const FAssetCanonicalResaveSelection& Selection = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetCanonicalResavePlan;

	ASSETCORE_API auto ApplyAssetCanonicalResaves(
		FAssetCanonicalResavePlan Plan,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCanonicalResaveApplyOptions& Options = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetCanonicalResaveApplyResult;

	ASSETCORE_API auto SerializeAssetCanonicalResavePlanReport(
		const FAssetCanonicalResavePlan& Plan) -> std::string;
	ASSETCORE_API auto SerializeAssetCanonicalResaveApplyReport(
		const FAssetCanonicalResaveApplyResult& Result) -> std::string;
}

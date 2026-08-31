#pragma once

#include "AssetMaintenanceAPI.h"
#include "AssetMaintenance/CompatibilityAudit.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetPackageMigrationReportSchemaVersion = 1;

	enum class EAssetPackageMigrationStatus : uint8
	{
		AlreadyV8,
		Ready,
		Converted,
		Unsupported,
		Invalid,
		Stale,
		Failed,
		Cancelled,
	};

	struct FAssetPackageMigrationClosureFingerprint
	{
		uint64 MainBytes = 0;
		FXxHash128 MainHash;
		bool bBulkExists = false;
		uint64 BulkBytes = 0;
		FXxHash128 BulkHash;

		auto operator==(const FAssetPackageMigrationClosureFingerprint&) const -> bool = default;
	};

	struct FAssetPackageMigrationRecord
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		uint32 SourceFormatVersion = 0;
		EAssetPackageMigrationStatus Status = EAssetPackageMigrationStatus::Invalid;
		FAssetPackageMigrationClosureFingerprint Source;
		FAssetPackageMigrationClosureFingerprint Target;
		std::string DiagnosticCode;
		std::string Diagnostic;
	};

	enum class EAssetPackageMigrationPlanStatus : uint8 { Completed, Cancelled };

	struct FAssetPackageMigrationPlan
	{
		EAssetPackageMigrationPlanStatus Status = EAssetPackageMigrationPlanStatus::Completed;
		uint32 TargetFormatVersion = 0;
		std::vector<FAssetPackageMigrationRecord> Packages;
	};

	enum class EAssetPackageMigrationApplyStatus : uint8
	{
		Succeeded,
		Partial,
		Cancelled,
		Blocked,
		Failed,
		RecoveryRequired,
	};

	enum class EAssetPackageMigrationApplyPhase : uint8
	{
		Revalidate,
		Convert,
		PublishBulk,
		PublishMain,
		Verify,
	};

	struct FAssetPackageMigrationApplyOptions
	{
		std::function<bool(EAssetPackageMigrationApplyPhase, size_t)> ShouldFail;
	};

	struct FAssetPackageMigrationApplyResult
	{
		EAssetPackageMigrationApplyStatus Status = EAssetPackageMigrationApplyStatus::Failed;
		FAssetPackageMigrationPlan Plan;
		std::vector<std::string> ChangedPaths;
		std::string Diagnostic;
	};

	ASSETMAINTENANCE_API auto PlanAssetPackageMigrationV8(
		std::span<const FAssetPackageCompatibilityProbeInput> Inputs,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageMigrationPlan;

	ASSETMAINTENANCE_API auto ApplyAssetPackageMigrationV8(
		FAssetPackageMigrationPlan Plan,
		const FAssetPackageMigrationApplyOptions& Options = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageMigrationApplyResult;

	ASSETMAINTENANCE_API auto SerializeAssetPackageMigrationPlanReport(
		const FAssetPackageMigrationPlan& Plan) -> std::string;
	ASSETMAINTENANCE_API auto SerializeAssetPackageMigrationApplyReport(
		const FAssetPackageMigrationApplyResult& Result) -> std::string;
}

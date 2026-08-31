#pragma once

#include "AssetMaintenanceAPI.h"
#include "AssetRegistry/PackageTypes.h"
#include "DObject/PackageFormat.h"

namespace Durin::Asset
{
	inline constexpr uint32 PackageFormatMigrationReportSchemaVersion = 1;

	struct FPackageClosureFingerprint
	{
		FAssetPackageFingerprint Main;
		FAssetPackageFingerprint Bulk;
		bool bHasBulk = false;
		auto operator==(const FPackageClosureFingerprint&) const -> bool = default;
	};

	struct FPackageFormatMigrationInput
	{
		FPackagePath PackagePath;
		std::filesystem::path MainPath;
		std::filesystem::path BulkPath;
	};

	enum class EPackageFormatMigrationStatus : uint8
	{
		Ready,
		Blocked,
		Migrated,
		Stale,
		Failed,
		Cancelled,
	};

	struct FPackageFormatMigrationItem
	{
		FPackageFormatMigrationInput Input;
		FPackageClosureFingerprint SourceFingerprint;
		FPackageClosureFingerprint TargetFingerprint;
		EPackageFormatMigrationStatus Status = EPackageFormatMigrationStatus::Blocked;
		std::string Diagnostic;
	};

	enum class EPackageFormatMigrationPlanStatus : uint8 { Completed, Cancelled };
	struct FPackageFormatMigrationPlan
	{
		EPackageFormatMigrationPlanStatus Status = EPackageFormatMigrationPlanStatus::Completed;
		uint32 SourceFormatVersion = ObjectPackage::DastV8FormatVersion;
		uint32 TargetFormatVersion = ObjectPackage::DastV9FormatVersion;
		std::vector<FPackageFormatMigrationItem> Packages;
	};

	enum class EPackageFormatMigrationApplyStatus : uint8
	{
		Succeeded,
		Partial,
		Cancelled,
		Blocked,
		Failed,
		RecoveryRequired,
	};

	enum class EPackageFormatMigrationApplyPhase : uint8
	{
		Revalidate,
		Convert,
		PublishMain,
		PublishBulk,
		Verify,
	};

	struct FPackageFormatMigrationApplyOptions
	{
		std::function<bool(EPackageFormatMigrationApplyPhase, size_t)> ShouldFail;
	};

	struct FPackageFormatMigrationApplyResult
	{
		EPackageFormatMigrationApplyStatus Status = EPackageFormatMigrationApplyStatus::Failed;
		FPackageFormatMigrationPlan Plan;
		std::vector<std::string> ChangedPaths;
		std::string Diagnostic;
	};

	using FPackageFormatMigrationCancellationCheck = std::function<bool()>;

	ASSETMAINTENANCE_API auto PlanPackageFormatMigration(
		std::span<const FPackageFormatMigrationInput> Inputs,
		const FPackageFormatMigrationCancellationCheck& IsCancelled = {})
		-> FPackageFormatMigrationPlan;

	ASSETMAINTENANCE_API auto ApplyPackageFormatMigration(
		FPackageFormatMigrationPlan Plan,
		const FPackageFormatMigrationApplyOptions& Options = {},
		const FPackageFormatMigrationCancellationCheck& IsCancelled = {})
		-> FPackageFormatMigrationApplyResult;

	ASSETMAINTENANCE_API auto SerializePackageFormatMigrationPlanReport(
		const FPackageFormatMigrationPlan& Plan) -> std::string;
	ASSETMAINTENANCE_API auto SerializePackageFormatMigrationApplyReport(
		const FPackageFormatMigrationApplyResult& Result) -> std::string;
}

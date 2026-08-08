#pragma once

#include "AssetCompatibility.h"
#include "AssetPackageVersionPolicy.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetMigrationReportSchemaVersion = 1;

	enum class EAssetMigrationKind : uint8 { PackageFormat, AssetSchema };
	enum class EAssetMigrationRisk : uint8 { Lossless, DataLoss, Unknown };
	enum class EAssetMigrationResolutionStatus : uint8
	{
		Resolved,
		MissingChain,
		AmbiguousChain,
		CyclicChain,
		Cancelled
	};

	struct FAssetMigrationHandlerDescriptor
	{
		std::string HandlerId;
		EAssetMigrationKind Kind = EAssetMigrationKind::PackageFormat;
		uint32 SourceVersion = 0;
		uint32 TargetVersion = 0;
		EAssetMigrationRisk Risk = EAssetMigrationRisk::Unknown;

		auto operator==(const FAssetMigrationHandlerDescriptor&) const -> bool = default;
	};

	struct FAssetMigrationChainResolution
	{
		EAssetMigrationResolutionStatus Status = EAssetMigrationResolutionStatus::MissingChain;
		std::vector<FAssetMigrationHandlerDescriptor> Steps;
		std::string Diagnostic;
	};

	class ASSETCORE_API FAssetMigrationRegistry
	{
	public:
		auto Register(FAssetMigrationHandlerDescriptor Handler, std::string& OutError) -> bool;
		auto Validate(std::string& OutError) const -> bool;
		auto ResolveChain(
			EAssetMigrationKind Kind,
			uint32 SourceVersion,
			uint32 TargetVersion,
			const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {}) const
			-> FAssetMigrationChainResolution;
		auto GetHandlers() const -> std::span<const FAssetMigrationHandlerDescriptor> { return Handlers; }

	private:
		std::vector<FAssetMigrationHandlerDescriptor> Handlers;
	};

	enum class EAssetMigrationPackageStatus : uint8
	{
		Planned,
		Migrated,
		Skipped,
		Blocked,
		Failed,
		RolledBack
	};

	struct FAssetMigrationPackagePlan
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		FAssetPackageFingerprint Fingerprint;
		std::string ReportContentHash;
		EAssetMigrationPackageStatus Status = EAssetMigrationPackageStatus::Blocked;
		uint32 SourceFormatVersion = 0;
		uint32 TargetFormatVersion = AssetPackageMigrationWriterVersion;
		std::vector<FAssetMigrationHandlerDescriptor> Steps;
		std::vector<std::string> Diagnostics;
	};

	struct FAssetMigrationSelection
	{
		std::vector<std::string> Mounts;
		std::vector<FAssetPath> Packages;
	};

	enum class EAssetMigrationPlanStatus : uint8 { Completed, Cancelled };

	struct FAssetMigrationPlan
	{
		EAssetMigrationPlanStatus Status = EAssetMigrationPlanStatus::Completed;
		std::vector<FAssetMigrationPackagePlan> Packages;
	};

	enum class EAssetMigrationApplyStatus : uint8
	{
		Succeeded,
		Cancelled,
		Blocked,
		Failed,
		RolledBack,
		RecoveryRequired
	};

	enum class EAssetMigrationApplyPhase : uint8
	{
		LoadPackage,
		SerializePackage,
		StagePackage,
		PublishPackage,
		VerifyPackage,
		RollbackPackage
	};

	struct FAssetMigrationApplyOptions
	{
		std::function<bool(EAssetMigrationApplyPhase, size_t)> ShouldFail;
	};

	struct FAssetMigrationApplyResult
	{
		EAssetMigrationApplyStatus Status = EAssetMigrationApplyStatus::Failed;
		FAssetMigrationPlan Plan;
		std::vector<std::string> ChangedPaths;
		std::string Diagnostic;
	};

	ASSETCORE_API auto RegisterBuiltInAssetMigrations(
		FAssetMigrationRegistry& Registry,
		std::string& OutError) -> bool;

	ASSETCORE_API auto PlanAssetPackageMigrations(
		std::span<const FAssetPackageCompatibilityRecord> Records,
		const FAssetMigrationRegistry& Registry,
		const FAssetMigrationSelection& Selection = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetMigrationPlan;

	ASSETCORE_API auto SerializeAssetMigrationPlanReportV1(
		const FAssetMigrationPlan& Plan) -> std::string;

	ASSETCORE_API auto RecoverInterruptedAssetMigrations(std::string& OutError) -> bool;

	ASSETCORE_API auto ApplyAssetPackageMigrations(
		FAssetMigrationPlan Plan,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetMigrationApplyOptions& Options = {},
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetMigrationApplyResult;

	ASSETCORE_API auto SerializeAssetMigrationApplyReportV1(
		const FAssetMigrationApplyResult& Result) -> std::string;
}

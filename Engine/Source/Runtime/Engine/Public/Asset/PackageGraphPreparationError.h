#pragma once

#include "Asset/AssetDefinitions.h"
#include "DObject/AssetPath.h"
#include "DObject/PackageFormat.h"

namespace Durin
{
	enum class EPackageGraphPrepareReason : uint8
	{
		None, Reentrant, LoadServiceBusy, PackageBudget, RetainedByteBudget, Cancelled, InvalidSource,
		ProjectionFenced, ResidentTargetRequired, DuplicatePackage, ObjectBudget, InjectedDependencyFailure,
		DependencyProjectionFenced, PackageConstruction, DefaultInnerBudget, DependencyIdentityChanged,
		Allocation, CallbackException, Reader, LinkerValidation, ClassNotAdmitted, DependencyLoad,
		DependencyNotResident, Skeleton, ApplyValues, GraphValidation, ResourceRevalidation,
		LiveOperationRejected,
	};

	// ValuesPrepared does not mean runtime products or reference publication are ready.
	enum class EPackageGraphPrepareStatus : uint8
	{
		ValuesPrepared,
		Unsupported,
		InvalidClosure,
		MissingDependency,
		BudgetExceeded,
		Stale,
		Cancelled,
		Busy,
	};

	struct FPackageGraphPrepareResult
	{
		EPackageGraphPrepareStatus Status = EPackageGraphPrepareStatus::ValuesPrepared;
		FPackagePath PackagePath;
		EPackageGraphPrepareReason Reason = EPackageGraphPrepareReason::None;
		std::string Subject;
		uint64 Actual = 0;
		uint64 Maximum = 0;
		std::optional<FObjectValidationError> GraphValidationCause;
		std::optional<FPreparedPackageResourceError> ResourceCause;
		std::shared_ptr<const FAssetResult> AssetCause;
		std::optional<ObjectPackage::FPackageReaderResult> ReaderCause;
		explicit operator bool() const { return Status == EPackageGraphPrepareStatus::ValuesPrepared; }
	};

	ENGINE_API auto FormatPackageGraphPrepareError(const FPackageGraphPrepareResult& Result) -> std::string;
}

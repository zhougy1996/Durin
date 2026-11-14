#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Asset/AssetWriteResult.h"
#include "DObject/PackageBulkStorage.h"
#include "DObject/Archive.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/PackagePersistence.h"

namespace Durin
{
	ENGINE_API auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes
	) -> FAssetWriteResult;
	ENGINE_API auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& SourcePackagePath,
		const FPackagePath& OutputPackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes
	) -> FAssetWriteResult;
	// Complete snapshots retain every selected value; Delta follows paired defaults.
	enum class EAssetPackageSaveMode : uint8 { Delta, Complete };

	// Encoding rules only. Cook preparation and scheduling belong to Asset/Cook.h.
	struct FAssetPackageSerializationOptions
	{
		bool bRetainEditorOnlyData = false;
		std::shared_ptr<const FObjectSaveOverrides> SaveOverrides;
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta;
	};

	// Content parameters shared by synchronous, asynchronous and batch saves.
	struct FAssetPackageSaveArgs
	{
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta;
	};

	struct FAssetPackageSaveContext
	{
		FAssetPackageSaveArgs Args;
		std::function<bool()> IsCancellationRequested;
		ENGINE_API auto SaveAsync(DPackage*, FAssetWriteResult& Admission) const -> Tasks::TTask<FAssetWriteResult>;
	};

	ENGINE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> ObjectPackage::FPackageWriterResult;
	ENGINE_API auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> ObjectPackage::FPackageWriterResult;
	struct FAssetBatchSaveResult
	{
		FAssetWriteResult Result;
		std::vector<FPackagePath> SavedPackages;
		FPackagePath FailedPackage;
		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};
	// Saves in input order. Stops at the first failure;
	// SavedPackages stay committed. FailedPackage may have Result.Effect on disk.
	ENGINE_API auto SavePackages(
		std::span<DPackage* const> Packages,
		const FAssetPackageSaveArgs& Args = {}
	) -> FAssetBatchSaveResult;
	ENGINE_API auto SavePackage(DPackage* Package, const FAssetPackageSaveArgs& Args = {}) -> FAssetWriteResult;
	ENGINE_API auto AdmitAssetPackageToCatalog(
		const FPackagePath& Path
	) -> FAssetWriteResult;
} // namespace Durin

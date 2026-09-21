#pragma once

#include "EngineAPI.h"
#include "Asset/AssetReadResult.h"
#include "Asset/AssetWriteResult.h"
#include "DObject/PackageBulkStorage.h"
#include "Asset/CookedAsset.h"
#include "DObject/Archive.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/PackagePersistence.h"

namespace Durin
{
	class FObjectGraphReplacement;
	enum class EAssetPackageSaveDomain : uint8 { Authored, Cooked };

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

	struct FAssetPackageSerializationOptions
	{
		EAssetPackageSaveDomain Domain = EAssetPackageSaveDomain::Authored;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::shared_ptr<const FObjectSaveOverrides> SaveOverrides;
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		std::vector<FPackageBulkStoragePayload>* EditorBulkDataStoragePayloads = nullptr;
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta;
	};

	enum class EAssetBundleSavePhase : uint8
	{
		CreateDirectories,
		PublishCompanion,
		StagePackage,
		PublishPackage,
		PublishRootPackage,
		PublishRegistry
	};

	struct FAssetBundleSaveOptions
	{
		DPackage* RootPackage = nullptr;
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
		// Applies only to the current package; earlier successful saves are final.
		// Ordinary saves retain the existing committed-content/projection-pending policy.
		bool bRollbackOnRegistryFailure = false;
		// Admits only private packages owned by this prepared publication operation.
		const FObjectGraphReplacement* PreparedPublication = nullptr;
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta;
	};

	struct FAssetPackageSaveContext
	{
		FAssetBundleSaveOptions Options;
		EPackageSaveFlags Flags = SAVE_None;
		FTaskCancellationToken Cancellation;
		ENGINE_API auto SaveAsync(DPackage*, FAssetWriteResult& Admission) const -> Tasks::TTask<FAssetWriteResult>;
	};
	// Owner-thread coordinator for private graph persistence. After IsReady(),
	// prepare graph replacement and call Commit only inside its TryCommit callback.
	// Staging never changes live files or publishes objects. Dropping a ready save
	// discards its stages. A failed Commit retains the ordinary rollback semantics.
	class FPreparedAssetSave final
	{
	public:
		ENGINE_API static auto Begin(DPackage* Package, const FAssetBundleSaveOptions& Options,
			FAssetWriteResult& Admission) -> std::shared_ptr<FPreparedAssetSave>;
		ENGINE_API ~FPreparedAssetSave();
		ENGINE_API auto IsReady() const -> bool;
		ENGINE_API auto Commit(const FObjectGraphReplacement& Publication) -> FAssetWriteResult;
	private:
		FPreparedAssetSave();
		struct FState;
		std::unique_ptr<FState> State;
	};
	using FAsyncPackageSaveSink = std::function<void(const FPackagePath&, const FAssetWriteResult&)>;
	ENGINE_API auto SetAsyncPackageSaveSink(FAsyncPackageSaveSink Sink) -> void;
	namespace AssetPrivate { ENGINE_API auto SetAsyncSavePublicationFailureForTests(bool bFail) -> void; }

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
	// Saves in input order, with RootPackage last. Stops at the first failure;
	// SavedPackages stay committed. FailedPackage may have Result.Effect on disk.
	// PreparedPublication is restricted to a single-package call.
	ENGINE_API auto SavePackages(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}
	) -> FAssetBatchSaveResult;
	ENGINE_API auto SavePackage(DPackage* Package, EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta) -> FAssetWriteResult;
	ENGINE_API auto SavePackage(DPackage* Package, EPackageSaveFlags Flags,
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta) -> FAssetWriteResult;
	ENGINE_API auto AdmitAssetPackageToCatalog(
		const FPackagePath& Path
	) -> FAssetWriteResult;
} // namespace Durin

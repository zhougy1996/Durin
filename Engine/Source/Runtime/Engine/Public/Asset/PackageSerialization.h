#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"
#include "Asset/EditorBulkDataStorageTypes.h"
#include "Asset/CookedAsset.h"
#include "DObject/Archive.h"
#include "DObject/PackageSaveOverrides.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"

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
	) -> FAssetResult;
	ENGINE_API auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& SourcePackagePath,
		const FPackagePath& OutputPackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes
	) -> FAssetResult;
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
		std::vector<FEditorBulkDataStoragePayload>* EditorBulkDataStoragePayloads = nullptr;
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

	class FAsyncPackageSave;
	struct FAssetBundleSaveOptions
	{
		DPackage* RootPackage = nullptr;
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
		// Transactions that also own live candidates can reject the entire closure.
		// Ordinary saves retain the existing committed-content/projection-pending policy.
		bool bRollbackOnRegistryFailure = false;
		// Admits only private packages owned by this prepared publication operation.
		const FObjectGraphReplacement* PreparedPublication = nullptr;
		EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta;
	};

	// Game-thread owner of a single-package save. Serialization stays on the
	// caller; workers only stage and verify detached bytes. Complete publishes
	// on the game thread. Destruction drains staging and discards uncommitted files.
	class ENGINE_API FAsyncPackageSave
	{
	public:
		static auto Begin(DPackage* Package, FAssetResult& OutResult) -> std::unique_ptr<FAsyncPackageSave>;
		~FAsyncPackageSave();
		auto IsReady() const -> bool;
		auto Complete(FAssetBundleSaveOptions Options = {}) -> FAssetResult;
	private:
		FAsyncPackageSave();
		struct FState;
		std::unique_ptr<FState> State;
	};

	ENGINE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> FAssetResult;
	ENGINE_API auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> FAssetResult;
	ENGINE_API auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}
	) -> FAssetResult;
	ENGINE_API auto SavePackage(DPackage* Package, EAssetPackageSaveMode Mode = EAssetPackageSaveMode::Delta) -> FAssetResult;
	ENGINE_API auto AdmitAssetPackageToCatalog(
		const FPackagePath& Path
	) -> FAssetResult;
} // namespace Durin

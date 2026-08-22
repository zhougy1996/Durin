#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Result.h"
#include "Asset/AuthoredBulkData.h"
#include "DObject/CoreDObject.h"

namespace Durin::Asset
{
	struct FAssetPackageSaveOptions
	{
	};

	ASSETCORE_API auto CanonicalizeAssetPackageForCook(
		std::span<const std::byte> Bytes,
		std::vector<std::byte>& OutBytes
	) -> FAssetResult;

	struct FAssetPackageSerializationOptions
	{
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		std::vector<FAuthoredBulkPayload>* AuthoredBulkPayloads = nullptr;
	};

	enum class EAssetBundleSavePhase : uint8
	{
		CreateDirectories,
		StagePackage,
		PublishPackage,
		PublishRootPackage,
		PublishRegistry
	};

	struct FAssetBundleSaveOptions
	{
		DPackage* RootPackage = nullptr;
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
	};

	ASSETCORE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		std::vector<std::byte>& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> FAssetResult;
	ASSETCORE_API auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}
	) -> FAssetResult;
	ASSETCORE_API auto AdmitAssetPackageToCatalog(
		const FAssetPath& Path
	) -> FAssetResult;
} // namespace Durin::Asset

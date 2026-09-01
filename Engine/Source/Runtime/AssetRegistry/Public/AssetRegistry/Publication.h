#pragma once

#include "AssetRegistry/References.h"

namespace Durin
{
	struct FAssetRegistryPublication
	{
		uint64 ExpectedRevision = 0;
		std::unordered_map<FPackagePath, FAssetData> Assets;
		std::vector<FAssetPackageReferenceEdge> ReferenceEdges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> ReferenceFingerprints;
		std::vector<FAssetRegistryResult> ReferenceErrors;
		bool bReferenceIndexComplete = false;
	};

	// Deterministic path-scoped projection update. Adds require an empty path,
	// replacements require an existing path, and removes require an existing path.
	struct FAssetRegistryDelta
	{
		uint64 ExpectedRevision = 0;
		std::vector<FAssetData> Adds;
		std::vector<FAssetData> Replaces;
		std::vector<FPackagePath> Removes;
		std::vector<FPackagePath> ReferenceInvalidations;
	};

	ASSETREGISTRY_API auto CaptureAssetRegistryPublication()
		-> FAssetRegistryPublication;
	ASSETREGISTRY_API auto PublishAssetRegistryPublication(
		FAssetRegistryPublication Publication) -> FAssetRegistryResult;
	ASSETREGISTRY_API auto PublishAssetRegistryDelta(FAssetRegistryDelta Delta)
		-> FAssetRegistryResult;
	ASSETREGISTRY_API auto FenceAssetRegistryProjection(
		std::span<const FPackagePath> Paths) -> void;
	ASSETREGISTRY_API auto ClearAssetRegistryProjectionFence(
		std::span<const FPackagePath> Paths) -> void;
	ASSETREGISTRY_API auto IsAssetRegistryProjectionFenced(
		const FPackagePath& Path) -> bool;
	ASSETREGISTRY_API auto CaptureAssetRegistryProjectionFences()
		-> std::vector<FPackagePath>;
	ASSETREGISTRY_API auto FlushAssetRegistryCaches() -> void;
	ASSETREGISTRY_API auto IsAssetRegistryCacheDirty() -> bool;
	ASSETREGISTRY_API auto GetAssetRegistryCacheWarning() -> std::string;
}

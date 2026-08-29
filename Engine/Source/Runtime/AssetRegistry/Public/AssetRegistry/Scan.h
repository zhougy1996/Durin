#pragma once

#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/RegistryCache.h"

namespace Durin::Asset
{
	struct FAssetRegistryScanCandidate
	{
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<Private::FRegistryCacheEntry> CacheEntries;
		FAssetRegistryScanStats Stats;
		std::vector<FAssetResult> Errors;
		std::string CacheWarning;
	};

	// Enumerates auto-scan mounts and constructs a complete metadata candidate.
	// It does not publish state or construct package objects.
	ASSETREGISTRY_API auto ScanMountedAssetMetadata(
		EAssetRegistryScanMode Mode,
		FAssetRegistryScanCandidate& OutCandidate) -> void;

	// Rebuilds catalog and reference projections from mounted packages, publishes
	// the complete pair against the captured revision, and refreshes both caches.
	ASSETREGISTRY_API auto RefreshAssetRegistry(
		EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental)
		-> FAssetCatalogRefreshResult;
	ASSETREGISTRY_API auto MarkAssetRegistryCachesDirty() -> void;
	ASSETREGISTRY_API auto FlushAssetRegistryCaches() -> void;
	ASSETREGISTRY_API auto IsAssetRegistryCacheDirty() -> bool;
	ASSETREGISTRY_API auto GetAssetRegistryCacheWarning() -> std::string;
}

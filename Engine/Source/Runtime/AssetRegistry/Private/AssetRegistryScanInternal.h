#pragma once

#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistryCacheInternal.h"

namespace Durin::Asset::Private
{
	struct FAssetRegistryScanCandidate
	{
		std::unordered_map<FPackagePath, FAssetData> Assets;
		std::vector<FRegistryCacheEntry> CacheEntries;
		FAssetRegistryScanStats Stats;
		std::vector<FAssetResult> Errors;
		std::string CacheWarning;
	};

	auto ScanMountedAssetMetadata(
		EAssetRegistryScanMode Mode,
		FAssetRegistryScanCandidate& OutCandidate) -> void;
}

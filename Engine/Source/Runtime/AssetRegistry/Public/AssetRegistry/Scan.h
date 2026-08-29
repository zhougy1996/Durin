#pragma once

#include "AssetRegistry/PackageHeader.h"

namespace Durin::Asset
{
	// Rebuilds catalog and reference projections from mounted packages, publishes
	// the complete pair against the captured revision, and refreshes both caches.
	ASSETREGISTRY_API auto RefreshAssetRegistry(
		EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental)
		-> FAssetCatalogRefreshResult;
}

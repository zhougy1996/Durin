#pragma once

#include "AssetRegistry/References.h"

namespace Durin::Asset
{
	struct FAssetRegistryPublication
	{
		uint64 ExpectedRevision = 0;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<FAssetPackageReferenceEdge> ReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> ReferenceFingerprints;
		std::vector<FAssetResult> ReferenceErrors;
		bool bReferenceIndexComplete = false;
	};

	ASSETREGISTRY_API auto CaptureAssetRegistryPublication()
		-> FAssetRegistryPublication;
	ASSETREGISTRY_API auto PublishAssetRegistryPublication(
		FAssetRegistryPublication Publication) -> FAssetResult;
	ASSETREGISTRY_API auto FlushAssetRegistryCaches() -> void;
	ASSETREGISTRY_API auto IsAssetRegistryCacheDirty() -> bool;
	ASSETREGISTRY_API auto GetAssetRegistryCacheWarning() -> std::string;
}

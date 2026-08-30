#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/References.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/PackageInspection.h"

namespace Durin::Asset
{
	ENGINE_API auto ExtractAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences
	) -> FAssetResult;

	ENGINE_API auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages
	) -> FAssetResult;
	ENGINE_API auto BuildCookReachability(
		const FAssetRegistrySnapshot& RegistrySnapshot,
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages
	) -> FAssetResult;
} // namespace Durin::Asset

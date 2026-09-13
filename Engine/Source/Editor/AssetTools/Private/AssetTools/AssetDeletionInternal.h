#pragma once

#include "AssetTools/AssetDeletion.h"

namespace Durin::AssetToolsPrivate
{
	auto GetDeleteContributorRevision() -> uint64;
	// Conservative metadata-only filter; custom contributors can claim arbitrary paths.
	auto MayOwnCompanionInRoots(const FAssetData& Data,
		std::span<const std::filesystem::path> NormalizedRoots) -> bool;
	auto InspectAssetCompanionFilesForDeletion(const FAssetData& Data,
		std::vector<std::filesystem::path>& OutFiles) -> FAssetResult;
}

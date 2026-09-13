#pragma once

#include "AssetTools/AssetDeletion.h"

namespace Durin::AssetToolsPrivate
{
	auto GetDeleteContributorRevision() -> uint64;
	auto InspectAssetCompanionFilesForDeletion(const FAssetData& Data,
		std::vector<std::filesystem::path>& OutFiles) -> FAssetResult;
}

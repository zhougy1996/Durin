#pragma once

#include "AssetTools/AssetDeletion.h"

namespace Durin::AssetToolsPrivate
{
	auto InspectAssetCompanionFilesForDeletion(const FAssetData& Data,
		std::vector<std::filesystem::path>& OutFiles) -> FAssetResult;
}

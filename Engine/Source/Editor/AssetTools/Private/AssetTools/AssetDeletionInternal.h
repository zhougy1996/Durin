#pragma once

#include "CoreMinimal.h"

#include "AssetTools/NativeAssetDeletion.h"

namespace Durin::AssetToolsPrivate
{
	auto GetDeleteContributorRevision() -> uint64;
	// Conservative metadata-only filter; custom contributors can claim arbitrary paths.
	auto MayOwnCompanionInRoots(const FPackageData& Data,
		std::span<const std::filesystem::path> NormalizedRoots) -> bool;
	auto InspectAssetCompanionFiles(const FPackageData& Data,
		std::vector<std::filesystem::path>& OutFiles, std::optional<UnifiedError::FError>* OutError = nullptr,
		bool* OutHasContributor = nullptr) -> bool;
}

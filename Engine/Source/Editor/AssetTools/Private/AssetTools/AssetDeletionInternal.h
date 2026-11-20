#pragma once

#include "CoreMinimal.h"

#include "AssetTools/NativeAssetDeletion.h"

namespace Durin::AssetToolsPrivate
{
	// Filter by the package sibling path before opening the current package header.
	auto MayOwnCompanionInRoots(const FPackageData& Data,
		std::span<const std::filesystem::path> NormalizedRoots) -> bool;
	auto InspectAssetCompanionFiles(const FPackageData& Data,
		std::vector<std::filesystem::path>& OutFiles, std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
}

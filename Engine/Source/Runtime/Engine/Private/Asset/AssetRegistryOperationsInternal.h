#pragma once

#include "AssetRegistry/Publication.h"

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Redirector.h"
#include "Asset/Mutation.h"
#include "Asset/Testing.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin
{
	struct FAssetRelocationState;
	struct FAssetRedirectorFixupState;
	class FAssetLoadService;
	class FAssetMutationCoordinator;

	auto RefreshSavedPackages(std::span<const FPackagePath> Paths) -> FAssetReadResult;
}

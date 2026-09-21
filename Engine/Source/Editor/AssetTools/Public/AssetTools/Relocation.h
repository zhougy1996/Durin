#pragma once

#include "Asset/AssetWriteResult.h"

#include "AssetToolsAPI.h"
#include "AssetTools/MutationTypes.h"
#include "Asset/MutationExtensions.h"

namespace Durin
{
	// Prepares, revalidates, and commits synchronously on the owner thread.
	ASSETTOOLS_API auto RelocateAssets(
		std::span<const FAssetRelocationMapping> Mappings
	) -> FAssetMutationResultDetails;
} // namespace Durin

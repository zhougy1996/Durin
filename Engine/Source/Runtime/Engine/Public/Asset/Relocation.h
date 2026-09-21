#pragma once

#include "Asset/AssetWriteResult.h"

#include "EngineAPI.h"
#include "Asset/MutationTypes.h"

namespace Durin
{
	struct FAssetRelocationMapping
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	// Prepares, revalidates, and commits synchronously on the owner thread.
	ENGINE_API auto RelocateAssets(
		std::span<const FAssetRelocationMapping> Mappings
	) -> FAssetMutationResultDetails;
} // namespace Durin

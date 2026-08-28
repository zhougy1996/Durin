#pragma once

#include "EngineAPI.h"
#include "Asset/MutationTypes.h"

namespace Durin::Asset
{
	struct FAssetRelocationMapping
	{
		FAssetPath SourcePath;
		FAssetPath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	ENGINE_API auto PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction
	) -> FAssetResult;
} // namespace Durin::Asset

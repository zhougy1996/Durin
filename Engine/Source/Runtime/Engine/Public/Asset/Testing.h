#pragma once

#include "Asset/AssetReadResult.h"

#include "EngineAPI.h"
#include "Asset/Mutation.h"

namespace Durin
{
	ENGINE_API auto FlushAssetCatalogSnapshotForTesting() -> void;
	ENGINE_API auto IsAssetCatalogSnapshotDirtyForTesting() -> bool;
	ENGINE_API auto GetAssetCatalogCacheWarningForTesting() -> std::string;
} // namespace Durin

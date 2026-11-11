#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetReadResult.h"
#include "DObject/AssetPath.h"
#include "EngineAPI.h"

namespace Durin::AssetPrivate
{
	ENGINE_API auto CheckPackageUnloadAllowed() -> FAssetReadResult;
	ENGINE_API auto CollectPackageExports(const std::unordered_set<FPackagePath>& Candidates) -> size_t;
}

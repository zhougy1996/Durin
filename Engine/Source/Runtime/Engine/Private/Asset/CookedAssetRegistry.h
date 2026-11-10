#pragma once

#include "CoreMinimal.h"
#include "Asset/CookedAsset.h"

namespace Durin::AssetPrivate
{
	// Reads only manifest-listed package headers; publishes the complete catalog.
	auto InitializeCookedAssetRegistry(const FAssetRuntimeConfiguration& Configuration) -> FAssetReadResult;
}

#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Asset/CookedAsset.h"

namespace Durin::AssetPrivate
{
	// Reads only manifest-listed package headers; publishes the complete catalog.
	auto InitializeCookedAssetRegistry(const FAssetRuntimeConfiguration& Configuration) -> std::expected<void, Durin::UnifiedError::FError>;
}

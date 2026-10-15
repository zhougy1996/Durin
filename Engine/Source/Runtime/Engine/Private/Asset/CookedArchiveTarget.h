#pragma once

#include "Asset/CookedAsset.h"
#include "Serialization/Archive.h"

namespace Durin::AssetPrivate
{
	inline auto CookedArchiveTarget(const FAssetRuntimeConfiguration& Configuration)
		-> FArchiveTarget
	{
		return {.Platform = std::string(GetTargetPlatformName(Configuration.GetCookTargetPlatform())), .Profile = "Game"};
	}
}

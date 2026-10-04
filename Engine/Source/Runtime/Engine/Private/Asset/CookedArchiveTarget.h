#pragma once

#include "Asset/CookedAsset.h"
#include "Serialization/Archive.h"

namespace Durin::AssetPrivate
{
	inline auto CookedArchiveTarget(const FAssetRuntimeConfiguration& Configuration)
		-> FArchiveTarget
	{
		return {.Platform = Configuration.GetCookTargetPlatform()
			== ECookTargetPlatform::MacOS ? "MacOS" : "Win64", .Profile = "Game"};
	}
}

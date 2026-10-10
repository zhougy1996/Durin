#pragma once

#include "CoreMinimal.h"
#include "Asset/CookedAsset.h"

namespace Durin::TextureCompressorPrivate
{
	inline auto IsSupportedBuildTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
	{
		return (Platform == ECookTargetPlatform::Win64 || Platform == ECookTargetPlatform::MacOS)
			&& Profile == ECookTargetProfile::Game;
	}
}

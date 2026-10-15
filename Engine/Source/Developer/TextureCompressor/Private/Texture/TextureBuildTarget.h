#pragma once

#include "CoreMinimal.h"
#include "TargetPlatform.h"

namespace Durin::TextureCompressorPrivate
{
	inline auto IsSupportedBuildTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
	{
		const auto Target = ResolveTargetPlatform(Platform);
		return Target && Profile == ECookTargetProfile::Game;
	}
}

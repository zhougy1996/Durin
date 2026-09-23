#pragma once

#include "Texture/VolumeTextureData.h"
#include "Texture/TextureBuildOutcome.h"

namespace Durin
{

	struct FVolumeTextureBuildInput
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

}

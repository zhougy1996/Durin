#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/VolumeTextureData.h"

namespace Durin
{
	struct FVolumeTextureSourceData;

	struct FVolumeTextureBuildInput
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = GetHostCookPlatform();
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		std::function<bool()> ShouldCancel;
	};

}

#endif

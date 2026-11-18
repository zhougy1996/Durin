#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"

#include "EngineAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"
#include "Texture/VolumeTexture.h"

namespace Durin::DerivedData { class FRequestOwner; }

namespace Durin
{
	// Captured source and cache policy; payload resolution is deferred until a miss.
	struct FVolumeTextureBuildRequest
	{
		FTextureSource Source;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = GetHostCookPlatform();
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
		DerivedData::FRequestOwner* RequestOwner = nullptr;
	};

	ENGINE_API auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTexturePlatformData, FTextureBuildOperationError>;

}

#endif

#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"

#include "EngineAPI.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/TextureCube.h"

namespace Durin::DerivedData { class FRequestOwner; }

namespace Durin
{
	// Engine-owned input and cache policy for detached construction.
	struct FTextureCubeBuildRequest
	{
		FTextureCubeNormalizationInput Input;
		ECookTargetPlatform TargetPlatform = GetHostCookPlatform();
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
		DerivedData::FRequestOwner* RequestOwner = nullptr;
	};

	struct FTextureCubeBuildValue
	{
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureCubePlatformData PlatformData;
	};

	ENGINE_API auto BuildTextureCubeDetached(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildOperationError>;

}

#endif

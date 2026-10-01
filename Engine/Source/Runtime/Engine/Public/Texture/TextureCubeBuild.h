#pragma once
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"

#include "EngineAPI.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	// Engine-owned input and cache policy for detached construction.
	struct FTextureCubeBuildRequest
	{
		FTextureCubeNormalizationInput Input;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	struct FTextureCubeResultApplicationContext
	{
		bool bMarkPackageDirty = true;
		bool bSourceDecoderInvoked = true;
		// PostLoad/rebuild consumes the installed source without replacing its storage.
		bool bPreserveSource = false;
	};

	struct FTextureCubeBuildValue
	{
		FTextureCubeCanonicalBuildInput CanonicalInput;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
	};

	ENGINE_API auto BuildTextureCubeDetached(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildOperationError>;
	ENGINE_API auto BuildTextureCubeSynchronously(DTextureCube& Texture, const FTextureCubeBuildRequest& Request, const FTextureCubeResultApplicationContext& Context)
		-> std::expected<void, FTextureBuildOperationError>;
}

#endif

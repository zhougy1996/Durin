#pragma once

#include "Texture/TextureBuildOperation.h"

#include "Asset/AssetCacheDiagnostic.h"

#include "Asset/DerivedDataCacheKeyProxy.h"
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

	enum class ETextureCubeBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// Detached derived-only product. Authored and normalized source stay separate.
	struct FTextureCubeBuildProduct
	{
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		FCacheKeyProxy DerivedDataKey;
		FAssetCacheDiagnostics PersistenceDiagnostic;
		FTextureCubeBuildDescriptor Builder;
		ETextureCubeBuildProductOrigin Origin = ETextureCubeBuildProductOrigin::Rebuilt;
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
		FTextureCubeBuildProduct Product;
	};

	ENGINE_API auto BuildTextureCubeDetached(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildOperationError>;
	ENGINE_API auto BuildTextureCubeSynchronously(DTextureCube& Texture, const FTextureCubeBuildRequest& Request, const FTextureCubeResultApplicationContext& Context)
		-> std::expected<void, FTextureBuildOperationError>;
}

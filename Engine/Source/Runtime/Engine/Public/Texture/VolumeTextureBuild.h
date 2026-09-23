#pragma once

#include "Texture/TextureBuildOperation.h"

#include "Asset/AssetCacheDiagnostic.h"

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "Texture/VolumeTextureBuildTypes.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	// Engine cache policy and borrowed source for one synchronous build.
	struct FVolumeTextureBuildRequest
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	enum class EVolumeTextureBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// Detached derived-only result; authored source and settings stay in Engine.
	struct FVolumeTextureBuildProduct
	{
		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
		FCacheKeyProxy DerivedDataKey;
		FAssetCacheDiagnostics PersistenceDiagnostic;
		EVolumeTextureBuildProductOrigin Origin = EVolumeTextureBuildProductOrigin::Rebuilt;
	};

	// Caller-owned result-application policy used only by Engine on the GameThread.
	struct FVolumeTextureResultApplicationContext
	{
		bool bMarkPackageDirty = true;
		bool bSourceDecoderInvoked = true;
		// PostLoad/rebuild consumes the installed source without replacing its storage.
		bool bPreserveSource = false;
	};

	ENGINE_API auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildProduct, FTextureBuildOperationError>;
	ENGINE_API auto BuildVolumeTextureSynchronously(DVolumeTexture& Texture, const FVolumeTextureBuildRequest& Request, const FVolumeTextureResultApplicationContext& Context)
		-> std::expected<void, FTextureBuildOperationError>;
}

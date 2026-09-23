#pragma once

#include "Texture/TextureBuildOperation.h"

#include "Asset/AssetCacheDiagnostic.h"

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "Texture/TextureBuildOutcome.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	// Borrows immutable normalized source for the duration of one synchronous
	// module invocation. Recipes must not retain the reference.
	struct FVolumeTextureBuildRequest
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	// Stable producer identity included in Engine-side diagnostics and contracts.
	struct FVolumeTextureBuildDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0;
		}
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
		FVolumeTextureBuildDescriptor Builder;
		EVolumeTextureBuildProductOrigin Origin = EVolumeTextureBuildProductOrigin::Rebuilt;
	};

	struct FVolumeTextureRecipeBuildRequest
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FVolumeTextureRecipeBuildProduct
	{
		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
	};

	// Caller-owned result-application policy used only by Engine on the GameThread.
	struct FVolumeTextureResultApplicationContext
	{
		bool bMarkPackageDirty = true;
		bool bSourceDecoderInvoked = true;
		// PostLoad/rebuild consumes the installed source without replacing its storage.
		bool bPreserveSource = false;
	};

	struct FVolumeTextureBuildValue
	{
		FVolumeTextureBuildProduct Product;
	};

	ENGINE_API auto BuildVolumeTextureDetached(const FVolumeTextureBuildRequest& Request)
		-> std::expected<FVolumeTextureBuildValue, FTextureBuildOperationError>;
	ENGINE_API auto BuildVolumeTextureSynchronously(DVolumeTexture& Texture, const FVolumeTextureBuildRequest& Request, const FVolumeTextureResultApplicationContext& Context)
		-> std::expected<void, FTextureBuildOperationError>;
}

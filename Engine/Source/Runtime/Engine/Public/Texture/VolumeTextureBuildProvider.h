#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	// Borrows immutable normalized source for the duration of one synchronous
	// provider invocation. Providers must not retain the reference.
	struct FVolumeTextureBuildRequest
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	// Stable producer identity included in Engine-side diagnostics and contracts.
	struct FVolumeTextureBuildProviderDescriptor
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
		std::string DerivedDataKey;
		std::string PersistenceDiagnostic;
		FVolumeTextureBuildProviderDescriptor Provider;
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
	};

	// Typed synchronous recipe seam implemented by the editor TextureBuild module.
	class IVolumeTextureBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.VolumeTextureBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetDescriptor() const
			-> FVolumeTextureBuildProviderDescriptor = 0;
		virtual auto Build(
			const FVolumeTextureRecipeBuildRequest& Request,
			FVolumeTextureRecipeBuildProduct& OutProduct,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeVolumeTextureBuildProvider(
		const FVolumeTextureBuildRequest& Request,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	ENGINE_API auto BuildVolumeTextureSynchronously(
		DVolumeTexture& Texture,
		const FVolumeTextureBuildRequest& Request,
		const FVolumeTextureResultApplicationContext& Context,
		std::string& OutError) -> bool;
}

#pragma once

#include <expected>

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Texture/Texture.h"
#include "Texture/TextureSource.h"
#include "Texture/VolumeTextureData.h"

#include "VolumeTexture.gen.h"

namespace Durin
{
	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Payload-backed input may require a synchronous read.
	ENGINE_API auto PrepareVolumeTextureSource(
		const FVolumeTextureSourceData& Value) -> std::expected<FTextureSource, std::string>;

	// Package-backed volume asset with owned updates and last-successful GPU publication.
	DCLASS()
	class DVolumeTexture : public DTexture
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DVolumeTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DVolumeTexture() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		// GameThread only. Assigns validated settings and cancels pending authored builds.
		ENGINE_API auto SetBuildSettings(
			FVolumeTextureBuildSettings Value) -> void;

		ENGINE_API auto CreateBuildInput() const -> FVolumeTextureSourceData;

		// Returns installed CPU data only; never loads bulk data or updates resources.
		auto GetPlatformData() const -> const FVolumeTexturePlatformData*
		{
			return PlatformData.get();
		}
		// Immutable input identity for uploads and CPU previews; replacement leaves existing readers valid.
		auto GetPlatformDataShared() const -> std::shared_ptr<const FVolumeTexturePlatformData> { return PlatformData; }
		auto HasPlatformData() const -> bool override
		{
			return PlatformData && PlatformData->IsValid();
		}
		// Adopts data already validated by the producer on GameThread; does not update resources.
		ENGINE_API auto SetPlatformData(
			std::unique_ptr<FVolumeTexturePlatformData> Data) -> void;

	protected:
		auto CreateRenderResourceCandidate(FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> override;

	private:
		auto ResetPlatformData() -> void override { PlatformData.reset(); }
		auto BuildPlatformDataForLoad() -> void override;
		auto LoadCookedPlatformData() -> bool override;

		DPROPERTY(EditorOnly)
		FVolumeTextureBuildSettings BuildSettings;

		std::shared_ptr<FVolumeTexturePlatformData> PlatformData;
	};
}

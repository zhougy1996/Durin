#pragma once

#include "CoreMinimal.h"

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Texture/TextureBuildOperation.h"
#include "Texture/Texture.h"
#include "Texture/TextureSource.h"
#include "Texture/VolumeTextureData.h"

#include "VolumeTexture.gen.h"

namespace Durin
{
	struct FVolumeTextureBuildRequest;

	// Prepares detached source on the caller thread; logs failures and returns nullopt.
	// Payload-backed input may require a synchronous read.
#if DURIN_WITH_EDITORONLY_DATA
	ENGINE_API auto PrepareVolumeTextureSource(
		const FVolumeTextureSourceData& Value) -> std::expected<FTextureSource, std::string>;

	// Package-backed volume asset with owned updates and last-successful GPU publication.
#endif

	DCLASS()
	class DVolumeTexture : public DTexture
	{
		GENERATED_BODY()

	public:
		using DTexture::CachePlatformData;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto CachePlatformData(const FVolumeTextureBuildRequest& Request,
			const FTextureBuildApplicationContext& Context = {}) -> std::expected<void, FTextureBuildOperationError>;
#endif
		ENGINE_API explicit DVolumeTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DVolumeTexture() override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API static auto AppendToClassSchema(FObjectCookContext& Context) -> void;

#if DURIN_WITH_EDITORONLY_DATA
		auto GetBuildSettings() const -> const FVolumeTextureBuildSettings& { return BuildSettings; }
		// GameThread only. Waits for authored builds before assigning validated settings.
		ENGINE_API auto SetBuildSettings(
			FVolumeTextureBuildSettings Value) -> void;
#endif

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
		auto CreateResource(FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> override;

	private:
		auto ResetPlatformData() -> void override { PlatformData.reset(); }
		auto BuildPlatformDataForLoad(bool bAsyncCache) -> void override;
		auto LoadCookedPlatformData(bool bAsync) -> void override;

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		FVolumeTextureBuildSettings BuildSettings;
#endif

		std::shared_ptr<FVolumeTexturePlatformData> PlatformData;
	};
}

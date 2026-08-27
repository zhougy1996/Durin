#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Texture/VolumeTexture.h"

#include "VolumeTextureImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FVolumeTextureImportDataState : FAssetImportDataState
	{
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;
		uint32 SliceWidth = 0;
		uint32 SliceHeight = 0;
		uint32 Depth = 0;
		uint32 TilesX = 0;
		uint32 TilesY = 0;
		auto operator==(const FVolumeTextureImportDataState&) const -> bool = default;
	};

	// Retains the atlas interpretation required to rebuild a VolumeTexture source.
	DCLASS()
	class DVolumeTextureImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DVolumeTextureImportData(
			const FObjectInitializer& ObjectInitializer);
		ASSETFORGEBUILTINS_API auto SetState(
			FVolumeTextureImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetVolumeTextureState() const
			-> FVolumeTextureImportDataState;
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;

	private:
		DPROPERTY()
		EVolumeTextureImportFormat ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas;

		DPROPERTY()
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;

		DPROPERTY()
		uint32 SliceWidth = 0;

		DPROPERTY()
		uint32 SliceHeight = 0;

		DPROPERTY()
		uint32 Depth = 0;

		DPROPERTY()
		uint32 TilesX = 0;

		DPROPERTY()
		uint32 TilesY = 0;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;
	};
}

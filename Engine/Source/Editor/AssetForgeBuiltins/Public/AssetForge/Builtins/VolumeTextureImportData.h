#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Texture/VolumeTexture.h"

#include "VolumeTextureImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FVolumeTextureImportDataState : AssetImport::FAssetImportDataState
	{
		EVolumeTextureImportFormat ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas;
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;
		uint32 SliceWidth = 0;
		uint32 SliceHeight = 0;
		uint32 Depth = 0;
		uint32 TilesX = 0;
		uint32 TilesY = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;

		auto operator==(const FVolumeTextureImportDataState&) const -> bool = default;
	};

	DCLASS()
	class DVolumeTextureImportData final : public AssetImport::DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DVolumeTextureImportData(
			const FObjectInitializer& ObjectInitializer);
		ASSETFORGEBUILTINS_API auto SetState(
			FVolumeTextureImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetVolumeTextureState() const
			-> FVolumeTextureImportDataState;
		auto GetState() const -> AssetImport::FAssetImportDataState override
		{
			return GetVolumeTextureState();
		}
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;
		ASSETFORGEBUILTINS_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> AssetImport::DAssetImportData* override;

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

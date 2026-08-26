#pragma once

#include "TextureBuildAPI.h"
#include "Texture/VolumeTexture.h"

namespace Durin::Asset
{
	// Detached normalized source and built platform value ready for atomic publication.
	struct FVolumeTextureBuildProduct
	{
		FVolumeTextureSourceData SourceData;
		FVolumeTextureBuildSettings Settings;
		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		bool bCacheHit = false;
	};

	TEXTUREBUILD_API auto BuildVolumeTexture(
		FVolumeTextureSourceData SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto PublishVolumeTextureProduct(
		DVolumeTexture& Texture,
		FVolumeTextureBuildProduct Product,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto MakeVolumeTextureDerivedDataKey(
		const DVolumeTexture& Texture, std::string& OutError) -> std::string;
	TEXTUREBUILD_API auto LoadVolumeTextureDerivedData(
		std::string_view Key,
		std::unique_ptr<FVolumeTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool;
}

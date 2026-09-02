#include "Texture/VolumeTextureBuildOperations.h"

#include "Texture/VolumeTextureBuilder.h"

namespace Durin
{
	auto BuildVolumeTexture(const FVolumeTextureRecipeBuildRequest& Request,
		FVolumeTextureRecipeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		const FVolumeTextureSourceData& SourceData = Request.SourceData.get();
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !SourceData.IsValid()
			|| SourceData.Format != Request.Settings.OutputFormat)
		{
			OutError = "Volume texture build source, settings, or target is incompatible.";
			return false;
		}
		auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
		if (!VolumeTextureBuilder::BuildMipChain(
			SourceData, Request.Settings, *PlatformData, OutError)) return false;
		OutProduct.PlatformData = std::move(PlatformData);
		OutError.clear();
		return true;
	}
}

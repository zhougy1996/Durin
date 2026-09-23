#include "Texture/VolumeTextureBuildOperations.h"

#include "Texture/VolumeTextureBuilder.h"

namespace Durin
{
	auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request) -> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError>
	{
		const FVolumeTextureSourceData& SourceData = Request.SourceData.get();
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !SourceData.IsValid()
			|| SourceData.Format != Request.Settings.OutputFormat)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
				"Volume texture build source, settings, or target is incompatible."});
		}
		auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
		if (auto Result = VolumeTextureBuilder::BuildMipChain(
			SourceData, Request.Settings, *PlatformData); !Result) return std::unexpected(std::move(Result.error()));
		return PlatformData;
	}
}

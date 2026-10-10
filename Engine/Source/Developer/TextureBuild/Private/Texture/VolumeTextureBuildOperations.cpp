#include "Texture/VolumeTextureBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Texture/VolumeTextureBuilder.h"

namespace Durin
{
	auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request) -> std::optional<FVolumeTexturePlatformData>
	{
		auto Fail = [](std::string_view Reason) -> std::optional<FVolumeTexturePlatformData>
		{
			DURIN_ERROR_CATEGORY("TextureBuild", "VolumeTexture build failed: {}", Reason);
			return std::nullopt;
		};
		const FVolumeTextureSourceData& SourceData = Request.SourceData.get();
		if ((Request.TargetPlatform != ECookTargetPlatform::Win64
				&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !SourceData.IsValid()
			|| SourceData.Format != Request.Settings.OutputFormat)
			return Fail("Volume texture build source, settings, or target is incompatible.");
		FVolumeTexturePlatformData PlatformData;
		if (auto Result = VolumeTextureBuilder::BuildMipChain(SourceData, Request.Settings, PlatformData); !Result)
			return Fail(Result.error().Diagnostic);
		return PlatformData;
	}
}

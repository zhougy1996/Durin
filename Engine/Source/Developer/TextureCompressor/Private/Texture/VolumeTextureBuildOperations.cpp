#include "Texture/VolumeTextureBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Texture/TextureBuildTarget.h"
#include "Texture/VolumeTextureMipGenerator.h"

namespace Durin
{
	auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request) -> std::expected<FVolumeTexturePlatformData, std::string>
	{
		if (Request.ShouldCancel && Request.ShouldCancel()) return std::unexpected("Texture build cancelled.");
		auto Fail = [](std::string_view Reason) -> std::expected<FVolumeTexturePlatformData, std::string>
		{
			return std::unexpected(std::string(Reason));
		};
		const FVolumeTextureSourceData& SourceData = Request.SourceData.get();
		if (!TextureCompressorPrivate::IsSupportedBuildTarget(Request.TargetPlatform, Request.TargetProfile)
			|| !SourceData.IsValid()
			|| SourceData.Format != Request.Settings.OutputFormat)
			return Fail("Volume texture build source, settings, or target is incompatible.");
		auto Result = VolumeTextureMipGenerator::GenerateMipChain(SourceData, Request.Settings, Request.ShouldCancel);
		if (!Result) return Fail(Result.error());
		return std::move(*Result);
	}
}

#include "Texture/VolumeTextureBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Texture/TextureBuildTarget.h"
#include "Texture/VolumeTextureMipGenerator.h"

namespace Durin
{
	auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request) -> std::optional<FVolumeTexturePlatformData>
	{
		auto Fail = [](std::string_view Reason) -> std::optional<FVolumeTexturePlatformData>
		{
			DURIN_ERROR_CATEGORY("TextureCompressor", "VolumeTexture build failed: {}", Reason);
			return std::nullopt;
		};
		const FVolumeTextureSourceData& SourceData = Request.SourceData.get();
		if (!TextureCompressorPrivate::IsSupportedBuildTarget(Request.TargetPlatform, Request.TargetProfile)
			|| !SourceData.IsValid()
			|| SourceData.Format != Request.Settings.OutputFormat)
			return Fail("Volume texture build source, settings, or target is incompatible.");
		auto Result = VolumeTextureMipGenerator::GenerateMipChain(SourceData, Request.Settings);
		if (!Result) return Fail(Result.error().Diagnostic);
		return std::move(*Result);
	}
}

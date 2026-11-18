#include "Texture/Texture2DBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Texture/TextureBuildTarget.h"
#include "Texture/TextureMipBuilder.h"

namespace Durin
{
	auto BuildTexture2D(const FTexture2DBuildInput& Request) -> std::expected<FTexturePlatformData, std::string>
	{
		if (Request.ShouldCancel && Request.ShouldCancel()) return std::unexpected("Texture build cancelled.");
		auto Fail = [](std::string_view Reason) -> std::expected<FTexturePlatformData, std::string>
		{
			return std::unexpected(std::string(Reason));
		};
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return Fail(FormatTexture2DInputError(Validation.error()));
		if (!TextureCompressorPrivate::IsSupportedBuildTarget(Request.TargetPlatform, Request.TargetProfile))
			return Fail("Texture2D build target is unsupported.");

		if (const auto Validation = ValidateTexture2DSourceMips(Request.SourceMips); !Validation)
			return Fail(FormatTexture2DInputError(Validation.error()));
		const bool bHasTransparency = TextureMipBuilder::AnalyzeTransparency(Request.SourceMips);
		auto Settings = Request.Settings;
		Settings.bSRGB = ResolveTexture2DSRGB(Settings);
		const TextureMipBuilder::FBuildExecutionOptions Execution{.ShouldCancel = Request.ShouldCancel};
		auto Built = TextureMipBuilder::BuildTextureMips({.SourceMips = Request.SourceMips, .Settings = Settings,
			.PixelFormat = TextureMipBuilder::SelectPixelFormat(Request.TargetPlatform, Settings.Usage, *Settings.bSRGB, bHasTransparency)}, &Execution);
		if (!Built) return Fail(Built.error());
		return std::move(Built->PlatformData);
	}
}

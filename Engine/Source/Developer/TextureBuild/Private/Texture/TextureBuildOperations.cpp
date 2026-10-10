#include "Texture/TextureBuildOperations.h"

#include "Logging/LogMacros.h"
#include "Texture/TextureBuilder.h"

namespace Durin
{
	auto BuildTexture2D(const FTexture2DBuildInput& Request) -> std::optional<FTexture2DBuildOutput>
	{
		auto Fail = [](std::string_view Reason) -> std::optional<FTexture2DBuildOutput>
		{
			DURIN_ERROR_CATEGORY("TextureBuild", "Texture2D build failed: {}", Reason);
			return std::nullopt;
		};
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return Fail(FormatTexture2DInputError(Validation.error()));
		if ((Request.TargetPlatform != ECookTargetPlatform::Win64
				&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
			|| Request.TargetProfile != ECookTargetProfile::Game)
			return Fail("Texture2D build target is unsupported.");

		if (const auto Validation = ValidateTexture2DSourceMips(Request.SourceMips); !Validation)
			return Fail(FormatTexture2DInputError(Validation.error()));
		const bool bHasTransparency = TextureBuilder::AnalyzeTransparency(Request.SourceMips);
		auto Settings = Request.Settings;
		Settings.bSRGB = ResolveTexture2DSRGB(Settings);
		auto Built = TextureBuilder::BuildMipChain({.SourceMips = Request.SourceMips, .Settings = Settings,
			.PixelFormat = TextureBuilder::SelectPixelFormat(Settings.Usage, *Settings.bSRGB, bHasTransparency)});
		if (!Built) return Fail(Built.error());
		return std::move(*Built);
	}
}

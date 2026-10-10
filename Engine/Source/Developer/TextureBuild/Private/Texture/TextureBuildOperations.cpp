#include "Texture/TextureBuildOperations.h"

#include "Texture/TextureBuilder.h"

namespace Durin
{
	auto BuildTexture2D(
		const FTexture2DBuildInput& Request,
		const FTexture2DBuildControl* ExecutionControl) -> std::expected<FTexture2DBuildOutput, FTexture2DBuildError>
	{
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if ((Request.TargetPlatform != ECookTargetPlatform::Win64
				&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::UnsupportedTarget});
		}

		if (const auto Validation = ValidateTexture2DSourceMips(Request.SourceMips); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		const TextureBuilder::FBuildExecutionControl Control{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{}};
		const auto Transparency = TextureBuilder::AnalyzeTransparency(Request.SourceMips, &Control);
		if (!Transparency) return std::unexpected(Transparency.error());
		auto Settings = Request.Settings;
		Settings.bSRGB = ResolveTexture2DSRGB(Settings);
		return TextureBuilder::BuildMipChain({.SourceMips = Request.SourceMips, .Settings = Settings,
			.PixelFormat = TextureBuilder::SelectPixelFormat(Settings.Usage, *Settings.bSRGB, *Transparency)}, &Control);
	}
}

#include "Texture/TextureBuildOperations.h"

#include "Texture/TextureBuilder.h"

namespace Durin
{
	auto BuildTexture2D(
		const FTexture2DRecipeBuildRequest& Request,
		const FTexture2DRecipeExecutionControl* ExecutionControl) -> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError>
	{
		FTexture2DRecipeBuildProduct Product;
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.error()});
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::UnsupportedTarget});
		}

		TextureBuilder::FBuildMipChainMetrics RecipeMetrics;
		const TextureBuilder::FBuildExecutionControl Control{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
				: std::function<bool()>{},
			.Metrics = &RecipeMetrics};
		const std::expected<void, FTexture2DBuildError> BuildResult = TextureBuilder::BuildMipChain(
			Request.SourceMips, Request.Settings.Usage,
			ResolveTexture2DSRGB(Request.Settings), Product.PlatformData,
			Request.Settings.MaxResolution, Request.Settings.CompressionQuality,
			Request.Settings.AlphaMipMode, Request.Settings.AlphaCoverageThreshold,
			&Control);
		if (!BuildResult) return std::unexpected(BuildResult.error());
		Product.Metrics = {
			.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds,
			.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds,
			.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes};
		return Product;
	}
}

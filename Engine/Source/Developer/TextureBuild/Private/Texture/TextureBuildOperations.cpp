#include "Texture/TextureBuildOperations.h"

#include "Texture/TextureBuilder.h"

namespace Durin
{
	auto BuildTexture2D(
		const FTexture2DRecipeBuildRequest& Request,
		FTexture2DRecipeBuildProduct& OutProduct,
		const FTexture2DRecipeExecutionControl* ExecutionControl) -> FTexture2DBuildResult
	{
		OutProduct = {};
		if (const auto Validation = ValidateTexture2DBuildSettings(Request.Settings); !Validation)
			return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.Error}};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			return {ETexture2DBuildStatus::Failed,
				{.Code = ETexture2DBuildError::UnsupportedTarget}};
		}

		TextureBuilder::FBuildMipChainMetrics RecipeMetrics;
		const TextureBuilder::FBuildExecutionControl Control{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
				: std::function<bool()>{},
			.Metrics = &RecipeMetrics};
		const FTexture2DBuildResult BuildResult = TextureBuilder::BuildMipChain(
			Request.SourceMips, Request.Settings.Usage,
			ResolveTexture2DSRGB(Request.Settings), OutProduct.PlatformData,
			Request.Settings.MaxResolution, Request.Settings.CompressionQuality,
			Request.Settings.AlphaMipMode, Request.Settings.AlphaCoverageThreshold,
			&Control);
		if (!BuildResult) return BuildResult;
		OutProduct.Metrics = {
			.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds,
			.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds,
			.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes};
		if (ExecutionControl && ExecutionControl->Metrics)
			*ExecutionControl->Metrics = OutProduct.Metrics;
		return {ETexture2DBuildStatus::Succeeded, {}};
	}
}

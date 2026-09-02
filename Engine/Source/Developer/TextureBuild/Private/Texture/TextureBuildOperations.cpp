#include "Texture/TextureBuildOperations.h"

#include "Texture/TextureBuilder.h"

namespace Durin
{
	auto BuildTexture2D(
		const FTexture2DRecipeBuildRequest& Request,
		FTexture2DRecipeBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DRecipeExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		const FTextureSourceData& SourceData = Request.SourceData.get();
		if (!SourceData.IsValid())
		{
			OutError = "Texture2D build requires valid normalized RGBA8 source data.";
			return false;
		}
		if (!ValidateTexture2DBuildSettings(Request.Settings, OutError)) return false;
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			OutError = "Texture2D build target is unsupported.";
			return false;
		}

		TextureBuilder::FBuildMipChainMetrics RecipeMetrics;
		const TextureBuilder::FBuildExecutionControl Control{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel
				: std::function<bool()>{},
			.Metrics = &RecipeMetrics};
		if (!TextureBuilder::BuildMipChain(SourceData, Request.Settings.Usage,
			ResolveTexture2DSRGB(Request.Settings), OutProduct.PlatformData, OutError,
			Request.Settings.MaxResolution, Request.Settings.CompressionQuality,
			Request.Settings.AlphaMipMode, Request.Settings.AlphaCoverageThreshold,
			&Control)) return false;
		OutProduct.Metrics = {
			.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds,
			.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds,
			.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes};
		if (ExecutionControl && ExecutionControl->Metrics)
			*ExecutionControl->Metrics = OutProduct.Metrics;
		OutError.clear();
		return true;
	}
}

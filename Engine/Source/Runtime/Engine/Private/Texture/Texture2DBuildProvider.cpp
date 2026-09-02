#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	auto ValidateTexture2DBuildSettings(
		const FTexture2DBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!IsValidTextureUsage(Settings.Usage)
			|| !IsValidTextureCompressionQuality(Settings.CompressionQuality)
			|| !IsValidTextureAlphaMipMode(Settings.AlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto ResolveTexture2DSRGB(const FTexture2DBuildSettings& Settings) -> bool
	{
		return Settings.bSRGB.value_or(GetDefaultTextureSRGB(Settings.Usage));
	}

	auto InvokeTexture2DBuildProvider(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		OutIdentity = {
			.ImportedDataIdentity = Request.SourceData.GetImportedDataIdentity(),
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		OutIdentity.Settings.bSRGB = ResolveTexture2DSRGB(Request.Settings);
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITexture2DBuildProvider>([&](ITexture2DBuildProvider& Provider) {
				OutIdentity.Provider = Provider.GetDescriptor();
				if (!OutIdentity.Provider.IsValid())
				{
					OutError = "The Texture2D build provider descriptor is invalid.";
					return false;
				}
				if (!Provider.Build(Request, OutProduct, OutError, ExecutionControl))
					return false;
				OutProduct.Provider = OutIdentity.Provider;
				if (ExecutionControl && ExecutionControl->Metrics)
					OutProduct.Metrics = *ExecutionControl->Metrics;
				return true;
			});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutProduct = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "The Texture2D build provider is unavailable.";
		else if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Multiple Texture2D build providers are registered.";
		else if (Invocation.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "The Texture2D build provider invocation failed.";
		else if (OutError.empty())
			OutError = "The Texture2D build provider failed without a diagnostic.";
		return false;
	}
}

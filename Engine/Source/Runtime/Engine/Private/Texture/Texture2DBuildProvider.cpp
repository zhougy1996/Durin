#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	auto InvokeTexture2DBuildProvider(
		FTexture2DBuildRequest Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		OutIdentity = {
			.SourceContentHashLow = Request.SourceContentHashLow,
			.SourceContentHashHigh = Request.SourceContentHashHigh,
			.Settings = Request.Settings,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITexture2DBuildProvider>([&](ITexture2DBuildProvider& Provider) {
				OutIdentity.Provider = Provider.GetDescriptor();
				if (!OutIdentity.Provider.IsValid())
				{
					OutError = "The Texture2D build provider descriptor is invalid.";
					return false;
				}
				return Provider.Build(
					std::move(Request), OutProduct, OutError, ExecutionControl);
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

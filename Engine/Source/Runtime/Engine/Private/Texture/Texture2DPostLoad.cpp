#include "Texture/Texture2DPostLoad.h"

namespace Durin
{
	auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		const auto Result = FModularFeatureRegistry::Get().InvokeSingle<ITexture2DAuthoringFeature>(
			[&](ITexture2DAuthoringFeature& Feature) { return Feature.PostLoadUncooked(Texture, OutError); });
		if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
		if (Result.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "No uncooked Texture2D load policy is registered.";
		else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Texture2D authoring capability is ambiguous.";
		else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "Texture2D authoring provider failed.";
		return false;
	}
}

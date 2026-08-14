#include "Texture/TextureCubePostLoad.h"

namespace Durin
{
	auto InvokeTextureCubeUncookedPostLoadHandler(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		const auto Result = FModularFeatureRegistry::Get().InvokeSingle<ITextureCubeAuthoringFeature>(
			[&](ITextureCubeAuthoringFeature& Feature) { return Feature.PostLoadUncooked(Texture, OutError); });
		if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
		if (Result.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "No uncooked TextureCube load policy is registered.";
		else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "TextureCube authoring capability is ambiguous.";
		else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "TextureCube authoring provider failed.";
		return false;
	}
}

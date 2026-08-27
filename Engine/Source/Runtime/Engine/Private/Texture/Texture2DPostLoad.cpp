#include "Texture/Texture2DPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITexture2DPostLoadFeature>(
			[&](ITexture2DPostLoadFeature& Feature) {
				return Feature.PostLoadUncooked(Texture, OutError);
			},
			{
				.Unavailable = "No uncooked Texture2D load policy is registered.",
				.Ambiguous = "Texture2D uncooked post-load policy is ambiguous.",
				.VisitorFailed = "Texture2D uncooked post-load handler failed."},
			OutError);
	}
}

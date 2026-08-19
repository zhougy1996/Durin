#include "Texture/Texture2DPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITexture2DAuthoringFeature>(
			[&](ITexture2DAuthoringFeature& Feature) {
				return Feature.PostLoadUncooked(Texture, OutError);
			},
			{
				.Unavailable = "No uncooked Texture2D load policy is registered.",
				.Ambiguous = "Texture2D authoring capability is ambiguous.",
				.VisitorFailed = "Texture2D authoring provider failed."},
			OutError);
	}
}

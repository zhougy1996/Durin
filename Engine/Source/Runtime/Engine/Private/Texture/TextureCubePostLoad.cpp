#include "Texture/TextureCubePostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeTextureCubeUncookedPostLoadHandler(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITextureCubePostLoadFeature>(
			[&](ITextureCubePostLoadFeature& Feature) {
				return Feature.PostLoadUncooked(Texture, OutError);
			},
			{
				.Unavailable = "No uncooked TextureCube load policy is registered.",
				.Ambiguous = "TextureCube authoring capability is ambiguous.",
				.VisitorFailed = "TextureCube authoring provider failed."},
			OutError);
	}
}

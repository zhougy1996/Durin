#include "Texture/VolumeTexturePostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeVolumeTextureUncookedPostLoadHandler(
		DVolumeTexture& Texture, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<IVolumeTextureAuthoringFeature>(
			[&](IVolumeTextureAuthoringFeature& Feature) {
				return Feature.PostLoadUncooked(Texture, OutError);
			}, {.Unavailable = "No uncooked volume texture load policy is registered.",
				.Ambiguous = "Volume texture authoring capability is ambiguous.",
				.VisitorFailed = "Volume texture authoring provider failed."}, OutError);
	}
}

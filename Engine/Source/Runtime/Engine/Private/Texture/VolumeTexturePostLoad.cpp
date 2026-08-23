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

	auto TryInvokeVolumeTextureInterchangeRecovery(
		DVolumeTexture& Texture, std::string& OutError) -> std::optional<bool>
	{
		const auto Result = FModularFeatureRegistry::Get().InvokeSingle<
			IVolumeTextureInterchangeRecoveryFeature>(
			[&](IVolumeTextureInterchangeRecoveryFeature& Feature) {
				return Feature.RecoverUncooked(Texture, OutError);
			});
		if (Result.Status == EFeatureInvokeStatus::Unavailable) return std::nullopt;
		if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value)
			return *Result.Value;
		if (Result.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "Volume texture Interchange recovery capability is ambiguous.";
		else OutError = "Volume texture Interchange recovery provider failed.";
		return false;
	}
}

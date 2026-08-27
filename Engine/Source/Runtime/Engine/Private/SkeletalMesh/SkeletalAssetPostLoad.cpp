#include "SkeletalMesh/SkeletalAssetPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view SkeletalDerivedDataAmbiguousMessage =
			"Skeletal derived-data capability is ambiguous.";
		constexpr std::string_view SkeletalDerivedDataVisitorFailedMessage =
			"Skeletal derived-data provider failed.";
	}

	auto InvokeSkeletalMeshUncookedPostLoad(
		DSkeletalMesh& Mesh,
		std::string& OutMessage) -> bool
	{
		return Private::InvokeSingleModularFeature<ISkeletalDerivedDataFeature>(
			[&](ISkeletalDerivedDataFeature& Feature) {
				return Feature.PostLoadUncooked(Mesh, OutMessage);
			},
			{
				.Unavailable = "No uncooked SkeletalMesh build policy is registered.",
				.Ambiguous = SkeletalDerivedDataAmbiguousMessage,
				.VisitorFailed = SkeletalDerivedDataVisitorFailedMessage},
			OutMessage);
	}

	auto InvokeAnimationClipUncookedPostLoad(
		DAnimationClip& Clip,
		std::string& OutMessage) -> bool
	{
		return Private::InvokeSingleModularFeature<ISkeletalDerivedDataFeature>(
			[&](ISkeletalDerivedDataFeature& Feature) {
				return Feature.PostLoadUncooked(Clip, OutMessage);
			},
			{
				.Unavailable = "No uncooked AnimationClip build policy is registered.",
				.Ambiguous = SkeletalDerivedDataAmbiguousMessage,
				.VisitorFailed = SkeletalDerivedDataVisitorFailedMessage},
			OutMessage);
	}

}

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

	auto InvokeSkeletalMeshUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return Private::InvokeSingleModularFeature<ISkeletalDerivedDataFeature>(
			[&](ISkeletalDerivedDataFeature& Feature) {
				return Feature.LoadSkeletalMeshPayload(Key, Context, OutPayload, OutMessage);
			},
			{
				.Unavailable = "No uncooked SkeletalMesh payload policy is registered.",
				.Ambiguous = SkeletalDerivedDataAmbiguousMessage,
				.VisitorFailed = SkeletalDerivedDataVisitorFailedMessage},
			OutMessage);
	}

	auto InvokeAnimationClipUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return Private::InvokeSingleModularFeature<ISkeletalDerivedDataFeature>(
			[&](ISkeletalDerivedDataFeature& Feature) {
				return Feature.LoadAnimationClipPayload(Key, Context, OutPayload, OutMessage);
			},
			{
				.Unavailable = "No uncooked AnimationClip payload policy is registered.",
				.Ambiguous = SkeletalDerivedDataAmbiguousMessage,
				.VisitorFailed = SkeletalDerivedDataVisitorFailedMessage},
			OutMessage);
	}
}

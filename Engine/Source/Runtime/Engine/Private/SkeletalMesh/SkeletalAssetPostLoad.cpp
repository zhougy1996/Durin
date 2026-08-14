#include "SkeletalMesh/SkeletalAssetPostLoad.h"

namespace Durin
{
	namespace
	{
		template<typename F>
		auto InvokeSkeletalFeature(F&& Visitor, std::string_view UnavailableMessage, std::string& OutMessage) -> bool
		{
			const auto Result = FModularFeatureRegistry::Get().InvokeSingle<ISkeletalDerivedDataFeature>(
				std::forward<F>(Visitor));
			if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
			if (Result.Status == EFeatureInvokeStatus::Unavailable) OutMessage = UnavailableMessage;
			else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
				OutMessage = "Skeletal derived-data capability is ambiguous.";
			else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
				OutMessage = "Skeletal derived-data provider failed.";
			return false;
		}
	}

	auto InvokeSkeletalMeshUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return InvokeSkeletalFeature([&](ISkeletalDerivedDataFeature& Feature) {
			return Feature.LoadSkeletalMeshPayload(Key, Context, OutPayload, OutMessage);
		}, "No uncooked SkeletalMesh payload policy is registered.", OutMessage);
	}

	auto InvokeAnimationClipUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return InvokeSkeletalFeature([&](ISkeletalDerivedDataFeature& Feature) {
			return Feature.LoadAnimationClipPayload(Key, Context, OutPayload, OutMessage);
		}, "No uncooked AnimationClip payload policy is registered.", OutMessage);
	}
}

#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	struct FAnimationClipPayloadData;
	struct FSkeletalMeshPayloadData;
	struct FSkeletalPayloadSerializationContext;

	class ISkeletalDerivedDataFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.SkeletalDerivedData";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto LoadSkeletalMeshPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FSkeletalMeshPayloadData& OutPayload,
			std::string& OutMessage) -> bool = 0;
		virtual auto LoadAnimationClipPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FAnimationClipPayloadData& OutPayload,
			std::string& OutMessage) -> bool = 0;
	};
	ENGINE_API auto InvokeSkeletalMeshUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
	ENGINE_API auto InvokeAnimationClipUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
}

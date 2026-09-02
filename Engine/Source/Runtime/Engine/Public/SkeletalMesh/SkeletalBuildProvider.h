#pragma once

#include "SkeletalMesh/SkeletalDerivedData.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	struct FSkeletalBuildProviderDescriptor
	{
		std::string SkeletalMeshProducerIdentity;
		uint32 SkeletalMeshProducerVersion = 0;
		std::string AnimationClipProducerIdentity;
		uint32 AnimationClipProducerVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !SkeletalMeshProducerIdentity.empty()
				&& SkeletalMeshProducerVersion != 0
				&& !AnimationClipProducerIdentity.empty()
				&& AnimationClipProducerVersion != 0;
		}
	};

	struct FSkeletalMeshRecipeRequest
	{
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		FSkeletalPayloadSerializationContext Context;
		std::function<bool()> ShouldCancel;
	};

	struct FAnimationClipRecipeRequest
	{
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		FSkeletalPayloadSerializationContext Context;
		std::function<bool()> ShouldCancel;
	};

	struct FSkeletalMeshRecipeProduct
	{
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
	};

	struct FAnimationClipRecipeProduct
	{
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
	};

	class ISkeletalBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.SkeletalBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetDescriptor() const -> FSkeletalBuildProviderDescriptor = 0;
		virtual auto BuildSkeletalMesh(
			const FSkeletalMeshRecipeRequest& Request,
			FSkeletalMeshRecipeProduct& OutProduct,
			std::string& OutError) -> bool = 0;
		virtual auto BuildAnimationClip(
			const FAnimationClipRecipeRequest& Request,
			FAnimationClipRecipeProduct& OutProduct,
			std::string& OutError) -> bool = 0;
	};

}

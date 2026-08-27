#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DAnimationClip;
	class DSkeletalMesh;
	struct FAnimationClipPayloadData;
	struct FSkeletalMeshPayloadData;
	struct FSkeletalPayloadSerializationContext;

	class ISkeletalDerivedDataFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.SkeletalDerivedData";
		static constexpr uint32 FeatureVersion = 2;
		virtual auto PostLoadUncooked(
			DSkeletalMesh& Mesh,
			std::string& OutMessage) -> bool = 0;
		virtual auto PostLoadUncooked(
			DAnimationClip& Clip,
			std::string& OutMessage) -> bool = 0;
	};
	ENGINE_API auto InvokeSkeletalMeshUncookedPostLoad(
		DSkeletalMesh& Mesh,
		std::string& OutMessage) -> bool;
	ENGINE_API auto InvokeAnimationClipUncookedPostLoad(
		DAnimationClip& Clip,
		std::string& OutMessage) -> bool;
}

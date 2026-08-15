#pragma once

#include "Animation/AnimationClip.h"
#include "EngineAPI.h"
#include "Math/Transform.h"
#include "SkeletalMesh/SkeletalMesh.h"

#include <atomic>

namespace Durin
{
	inline constexpr uint64 MaximumSkeletalPosePaletteBytes =
		static_cast<uint64>(MaximumSkeletonBones) * sizeof(FMatrix4f);

	// Detached immutable inputs for value-only skeletal pose evaluation.
	struct FSkeletalAnimationBinding
	{
		std::string SkeletonCompatibilityIdentity;
		std::vector<int32> ParentIndices;
		std::vector<FTransform> ReferenceLocalTransforms;
		std::shared_ptr<const FAnimationClipPayloadData> ClipPayload;
		FMatrix MeshNodeBindTransform{1.0};
		FMatrix InverseMeshNodeBindTransform{1.0};
		std::vector<uint16> PaletteBoneIndices;
		std::vector<FMatrix4f> InverseBindMatrices;
		std::vector<FBox> InfluenceBounds;
	};

	// Complete mesh-palette-aligned publication that can outlive its producer.
	struct FSkeletalPosePalette
	{
		uint64 Revision = 0;
		float SampleTimeSeconds = 0.0f;
		std::string SkeletonCompatibilityIdentity;
		std::vector<FMatrix4f> Matrices;
		FBox LocalBounds;
	};

	// Reads reflected assets only while prospectively preparing detached inputs.
	ENGINE_API auto BuildSkeletalAnimationBinding(
		const DSkeletalMesh& Mesh,
		const DAnimationClip* Clip,
		FSkeletalAnimationBinding& OutBinding,
		std::string& OutError) -> bool;

	// Evaluates only detached value data and leaves OutCandidate unchanged on failure.
	ENGINE_API auto EvaluateSkeletalPose(
		const FSkeletalAnimationBinding& Binding,
		float SampleTimeSeconds,
		uint64 Revision,
		std::shared_ptr<const FSkeletalPosePalette>& OutCandidate,
		std::string& OutError) -> bool;

	// Sole mutable owner for one detached single-clip playback state.
	class ENGINE_API FSkeletalAnimationInstance
	{
	public:
		auto Bind(
			const DSkeletalMesh& Mesh,
			const DAnimationClip* Clip,
			std::string& OutError) -> bool;
		auto Unbind() -> void;

		auto Play(std::string& OutError) -> bool;
		auto Pause() -> void;
		auto Stop(std::string& OutError) -> bool;
		auto Seek(float TimeSeconds, std::string& OutError) -> bool;
		auto Tick(float DeltaSeconds, std::string& OutError) -> bool;

		auto SetLooping(bool bInLooping) -> void { bLooping = bInLooping; }
		auto IsLooping() const -> bool { return bLooping; }
		auto SetPlayRate(float InPlayRate, std::string& OutError) -> bool;
		auto GetPlayRate() const -> float { return PlayRate; }

		auto IsBound() const -> bool { return bBound; }
		auto HasClip() const -> bool { return bBound && Binding.ClipPayload != nullptr; }
		auto IsPlaying() const -> bool { return bPlaying; }
		auto GetTimeSeconds() const -> float { return TimeSeconds; }
		auto GetDurationSeconds() const -> float
		{
			return HasClip() ? Binding.ClipPayload->DurationSeconds : 0.0f;
		}
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetLatestPosePalette() const -> std::shared_ptr<const FSkeletalPosePalette>
		{
			return std::atomic_load_explicit(&LatestCandidate, std::memory_order_acquire);
		}

	private:
		auto NormalizeTime(double InTimeSeconds) const -> float;
		auto EvaluateAt(float InTimeSeconds, std::string& OutError)
			-> std::shared_ptr<const FSkeletalPosePalette>;

		FSkeletalAnimationBinding Binding;
		std::shared_ptr<const FSkeletalPosePalette> LatestCandidate;
		float TimeSeconds = 0.0f;
		float PlayRate = 1.0f;
		uint64 Revision = 0;
		bool bBound = false;
		bool bPlaying = false;
		bool bLooping = true;
	};
}

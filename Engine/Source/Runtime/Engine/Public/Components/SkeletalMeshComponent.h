#pragma once

#include "Animation/SkeletalAnimation.h"
#include "Components/PrimitiveComponent.h"

#include "SkeletalMeshComponent.gen.h"

namespace Durin
{
	// Owns one detached skeletal playback instance without creating a render proxy in S2.
	DCLASS()
	class DSkeletalMeshComponent final : public DPrimitiveComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkeletalMeshComponent(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto SetSkeletalMesh(DSkeletalMesh* InMesh, std::string& OutError) -> bool;
		ENGINE_API auto SetAnimationClip(DAnimationClip* InClip, std::string& OutError) -> bool;
		auto GetSkeletalMesh() const -> DSkeletalMesh* { return SkeletalMesh.Get(); }
		auto GetAnimationClip() const -> DAnimationClip* { return AnimationClip.Get(); }

		ENGINE_API auto Play(std::string& OutError) -> bool;
		ENGINE_API auto Pause() -> void;
		ENGINE_API auto Stop(std::string& OutError) -> bool;
		ENGINE_API auto Seek(float TimeSeconds, std::string& OutError) -> bool;
		ENGINE_API auto SetLooping(bool bInLooping) -> void;
		auto IsLooping() const -> bool { return bLooping; }
		ENGINE_API auto SetPlayRate(float InPlayRate, std::string& OutError) -> bool;
		auto GetPlayRate() const -> float { return PlayRate; }
		auto IsPlaying() const -> bool { return AnimationInstance.IsPlaying(); }
		auto GetPlaybackTimeSeconds() const -> float { return AnimationInstance.GetTimeSeconds(); }
		auto GetPlaybackRevision() const -> uint64 { return AnimationInstance.GetRevision(); }
		auto GetLatestPosePalette() const -> std::shared_ptr<const FSkeletalPosePalette>
		{
			return AnimationInstance.GetLatestPosePalette();
		}

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto BeginPlay() -> void override;
		ENGINE_API auto TickComponent(float DeltaSeconds) -> void override;
		ENGINE_API auto EndPlay() -> void override;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;

	private:
		auto ValidateProspectiveBinding(
			DSkeletalMesh* InMesh,
			DAnimationClip* InClip,
			std::string& OutError) const -> bool;
		auto RebindInstance(
			DSkeletalMesh* InMesh,
			DAnimationClip* InClip,
			std::string& OutError) -> bool;
		auto RebindCurrent(std::string& OutError) -> bool;

		DPROPERTY(Edit)
		TObjectPtr<DSkeletalMesh> SkeletalMesh;

		DPROPERTY(Edit)
		TObjectPtr<DAnimationClip> AnimationClip;

		DPROPERTY(Edit)
		bool bAutoPlay = true;

		DPROPERTY(Edit)
		bool bLooping = true;

		DPROPERTY(Edit)
		float PlayRate = 1.0f;

		FSkeletalAnimationInstance AnimationInstance;
	};
}

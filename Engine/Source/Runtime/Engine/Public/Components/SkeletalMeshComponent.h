#pragma once

#include "Animation/SkeletalAnimation.h"
#include "Components/PrimitiveComponent.h"

#include "SkeletalMeshComponent.gen.h"

namespace Durin
{
	// Owns one detached skeletal playback instance and publishes complete render proxies.
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
		ENGINE_API auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto SetMaterialByName(FName SlotName, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterialByName(FName SlotName) const -> DMaterialInterface*;
		ENGINE_API auto ResetMaterial(uint32 SlotIndex) -> bool;
		ENGINE_API auto ClearMaterialOverrides() -> bool;
		auto GetNumMaterials() const -> uint32
		{
			return SkeletalMesh ? SkeletalMesh->GetNumMaterialSlots() : 0;
		}

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
		auto PublishPoseDynamicData() -> void;
		ENGINE_API auto BuildMaterialRenderProxyBindingUpdate(
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool override;
		auto TrimTrailingNullOverrides() -> void;

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

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialInterface>> OverrideMaterials;

		FSkeletalAnimationInstance AnimationInstance;
		uint64 LastPublishedPoseRevision = 0;
		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;
	};
}

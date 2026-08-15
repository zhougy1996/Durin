#include "Components/SkeletalMeshComponent.h"

#include "AssetLoad.h"
#include "DObject/DurinPropertyTypes.h"
#include "Engine/Level.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Materials/DefaultMaterialService.h"
#include "Materials/MaterialInterface.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace Durin
{
	namespace
	{
		auto IsAppliedChange(const FPropertyChangedEvent& Event) -> bool
		{
			return Event.Phase != EPropertyChangePhase::Committed
				|| Event.Origin != EPropertyChangeOrigin::Edit;
		}
	}

	DSkeletalMeshComponent::DSkeletalMeshComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SetComponentTickGroup(ETickingGroup::PostPhysics);
		SetComponentTickEnabled(true);
	}

	auto DSkeletalMeshComponent::SetMaterial(
		uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool
	{
		if (!SkeletalMesh || !SkeletalMesh->GetMaterialSlot(SlotIndex)) return false;
		if (!InMaterial) return ResetMaterial(SlotIndex);
		if (SlotIndex >= OverrideMaterials.size()) OverrideMaterials.resize(SlotIndex + 1);
		if (OverrideMaterials[SlotIndex] == InMaterial) return true;
		OverrideMaterials[SlotIndex] = InMaterial;
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSkeletalMeshComponent::GetMaterial(uint32 SlotIndex) const
		-> DMaterialInterface*
	{
		const FSkeletalMeshMaterialSlotDefinition* Slot = SkeletalMesh
			? SkeletalMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (!Slot) return nullptr;
		if (SlotIndex < OverrideMaterials.size() && OverrideMaterials[SlotIndex])
			return OverrideMaterials[SlotIndex].Get();
		return Slot->DefaultMaterial.Get();
	}

	auto DSkeletalMeshComponent::SetMaterialByName(
		FName SlotName, DMaterialInterface* InMaterial) -> bool
	{
		if (!SkeletalMesh) return false;
		const auto* Slot = SkeletalMesh->FindMaterialSlot(SlotName);
		return Slot && SetMaterial(static_cast<uint32>(
			Slot - SkeletalMesh->GetMaterialSlots().data()), InMaterial);
	}

	auto DSkeletalMeshComponent::GetMaterialByName(FName SlotName) const
		-> DMaterialInterface*
	{
		if (!SkeletalMesh) return nullptr;
		const auto* Slot = SkeletalMesh->FindMaterialSlot(SlotName);
		return Slot ? GetMaterial(static_cast<uint32>(
			Slot - SkeletalMesh->GetMaterialSlots().data())) : nullptr;
	}

	auto DSkeletalMeshComponent::ResetMaterial(uint32 SlotIndex) -> bool
	{
		if (!SkeletalMesh || !SkeletalMesh->GetMaterialSlot(SlotIndex)
			|| SlotIndex >= OverrideMaterials.size() || !OverrideMaterials[SlotIndex]) return false;
		OverrideMaterials[SlotIndex] = nullptr;
		TrimTrailingNullOverrides();
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DSkeletalMeshComponent::ClearMaterialOverrides() -> bool
	{
		if (OverrideMaterials.empty()) return false;
		OverrideMaterials.clear();
		++MaterialComponentRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DSkeletalMeshComponent::TrimTrailingNullOverrides() -> void
	{
		while (!OverrideMaterials.empty() && !OverrideMaterials.back())
			OverrideMaterials.pop_back();
	}

	auto DSkeletalMeshComponent::ValidateProspectiveBinding(
		DSkeletalMesh* InMesh,
		DAnimationClip* InClip,
		std::string& OutError) const -> bool
	{
		if (!InMesh)
		{
			if (InClip)
			{
				OutError = "A skeletal animation clip requires a skeletal mesh.";
				return false;
			}
			OutError.clear();
			return true;
		}
		FSkeletalAnimationBinding Candidate;
		return BuildSkeletalAnimationBinding(*InMesh, InClip, Candidate, OutError);
	}

	auto DSkeletalMeshComponent::RebindInstance(
		DSkeletalMesh* InMesh,
		DAnimationClip* InClip,
		std::string& OutError) -> bool
	{
		if (!std::isfinite(PlayRate) || PlayRate < 0.0f)
		{
			OutError = "Skeletal-mesh component play rate must be finite and non-negative.";
			return false;
		}
		if (!IsRegistered()) return ValidateProspectiveBinding(InMesh, InClip, OutError);
		if (!InMesh)
		{
			if (InClip)
			{
				OutError = "A skeletal animation clip requires a skeletal mesh.";
				return false;
			}
			AnimationInstance.Unbind();
			OutError.clear();
			return true;
		}
		const bool bWasPlaying = AnimationInstance.IsPlaying();
		if (!AnimationInstance.Bind(*InMesh, InClip, OutError)) return false;
		AnimationInstance.SetLooping(bLooping);
		const bool bAppliedPlayRate = AnimationInstance.SetPlayRate(PlayRate, OutError);
		check(bAppliedPlayRate);
		(void)bAppliedPlayRate;
		if (bWasPlaying && InClip && !AnimationInstance.Play(OutError)) return false;
		OutError.clear();
		return true;
	}

	auto DSkeletalMeshComponent::RebindCurrent(std::string& OutError) -> bool
	{
		return RebindInstance(SkeletalMesh.Get(), AnimationClip.Get(), OutError);
	}

	auto DSkeletalMeshComponent::PublishPoseDynamicData() -> void
	{
		const std::shared_ptr<const FSkeletalPosePalette> Pose = GetLatestPosePalette();
		if (!Pose || Pose->Revision == LastPublishedPoseRevision) return;
		if (IsRegistered())
		{
			if (IScene* Scene = GetRenderScene())
				Scene->UpdateSkeletalMeshDynamicData(GetPrimitiveSceneId(), Pose);
#if DURIN_WITH_EDITOR
			NotifyEditorPickingMutation();
#endif
		}
		LastPublishedPoseRevision = Pose->Revision;
	}

	auto DSkeletalMeshComponent::SetSkeletalMesh(
		DSkeletalMesh* InMesh,
		std::string& OutError) -> bool
	{
		if (SkeletalMesh.Get() == InMesh)
		{
			OutError.clear();
			return true;
		}
		if (!RebindInstance(InMesh, AnimationClip.Get(), OutError)) return false;
		SkeletalMesh = InMesh;
		++MaterialComponentRevision;
		LastPublishedPoseRevision = 0;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DSkeletalMeshComponent::SetAnimationClip(
		DAnimationClip* InClip,
		std::string& OutError) -> bool
	{
		if (AnimationClip.Get() == InClip)
		{
			OutError.clear();
			return true;
		}
		if (!RebindInstance(SkeletalMesh.Get(), InClip, OutError)) return false;
		AnimationClip = InClip;
		LastPublishedPoseRevision = 0;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DSkeletalMeshComponent::Play(std::string& OutError) -> bool
	{
		return AnimationInstance.Play(OutError);
	}

	auto DSkeletalMeshComponent::Pause() -> void
	{
		AnimationInstance.Pause();
	}

	auto DSkeletalMeshComponent::Stop(std::string& OutError) -> bool
	{
		if (!AnimationInstance.Stop(OutError)) return false;
		PublishPoseDynamicData();
		return true;
	}

	auto DSkeletalMeshComponent::Seek(float TimeSeconds, std::string& OutError) -> bool
	{
		if (!AnimationInstance.Seek(TimeSeconds, OutError)) return false;
		PublishPoseDynamicData();
		return true;
	}

	auto DSkeletalMeshComponent::SetLooping(bool bInLooping) -> void
	{
		if (bLooping == bInLooping) return;
		bLooping = bInLooping;
		AnimationInstance.SetLooping(bLooping);
		MarkPackageDirty();
	}

	auto DSkeletalMeshComponent::SetPlayRate(float InPlayRate, std::string& OutError) -> bool
	{
		if (!AnimationInstance.SetPlayRate(InPlayRate, OutError)) return false;
		if (PlayRate == InPlayRate) return true;
		PlayRate = InPlayRate == 0.0f ? 0.0f : InPlayRate;
		MarkPackageDirty();
		return true;
	}

	auto DSkeletalMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (OverrideMaterials.size() > MaximumSkeletalMeshMaterialSlots)
		{
			OutError = "Skeletal-mesh component material override count exceeds the supported limit.";
			return false;
		}
		TrimTrailingNullOverrides();
		if (!std::isfinite(PlayRate) || PlayRate < 0.0f)
		{
			OutError = "Skeletal-mesh component play rate must be finite and non-negative.";
			return false;
		}
		if (Asset::IsAssetMigrationLoad()) return true;
		if (IsSkeletalDerivedDataRepairLoadActive()
			&& SkeletalMesh
			&& (!SkeletalMesh->GetPayloadData()
				|| (AnimationClip && !AnimationClip->GetPayloadData())))
		{
			if (!SkeletalMesh->Validate(OutError)) return false;
			if (AnimationClip)
			{
				if (!AnimationClip->Validate(OutError)) return false;
				if (AnimationClip->GetSkeletonCompatibilityIdentity()
					!= SkeletalMesh->GetSkeletonCompatibilityIdentity())
				{
					OutError = "Animation clip is structurally incompatible with the skeletal mesh.";
					return false;
				}
			}
			OutError.clear();
			return true;
		}
		return ValidateProspectiveBinding(SkeletalMesh.Get(), AnimationClip.Get(), OutError);
	}

	auto DSkeletalMeshComponent::PreEditChangeProperty(
		FPropertyEditProposal& Proposal,
		std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.DraftRootProperty != Proposal.MemberProperty
			|| !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("PlayRate"))
		{
			const float Value = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (!std::isfinite(Value) || Value < 0.0f)
			{
				OutError = "Skeletal-mesh component play rate must be finite and non-negative.";
				return false;
			}
			return true;
		}
		if (Name != FName("SkeletalMesh") && Name != FName("AnimationClip")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			OutError = "Skeletal-mesh component asset property metadata is unavailable.";
			return false;
		}
		DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		if (Name == FName("SkeletalMesh"))
		{
			DSkeletalMesh* CandidateMesh = Cast<DSkeletalMesh>(Value);
			if (Value && !CandidateMesh)
			{
				OutError = "Selected asset is not a skeletal mesh.";
				return false;
			}
			return ValidateProspectiveBinding(CandidateMesh, AnimationClip.Get(), OutError);
		}
		DAnimationClip* CandidateClip = Cast<DAnimationClip>(Value);
		if (Value && !CandidateClip)
		{
			OutError = "Selected asset is not an animation clip.";
			return false;
		}
		return ValidateProspectiveBinding(SkeletalMesh.Get(), CandidateClip, OutError);
	}

	auto DSkeletalMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || !IsAppliedChange(Event)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("bLooping"))
		{
			AnimationInstance.SetLooping(bLooping);
			return;
		}
		if (Name == FName("PlayRate"))
		{
			std::string Error;
			if (!AnimationInstance.SetPlayRate(PlayRate, Error))
				DURIN_WARN("Skeletal-mesh component rejected edited play rate. (component: {}, reason: {})",
					GetObjectPath(), Error);
			return;
		}
		if (Name != FName("SkeletalMesh") && Name != FName("AnimationClip")) return;
		std::string Error;
		if (!RebindCurrent(Error))
			DURIN_WARN("Skeletal-mesh component failed to apply edited binding. (component: {}, reason: {})",
				GetObjectPath(), Error);
		LastPublishedPoseRevision = 0;
		MarkRenderStateDirty();
	}

	auto DSkeletalMeshComponent::OnRegister() -> void
	{
		Super::OnRegister();
		std::string Error;
		if (!RebindCurrent(Error))
			DURIN_WARN("Skeletal-mesh component registration could not prepare playback. (component: {}, reason: {})",
				GetObjectPath(), Error);
		else
		{
			LastPublishedPoseRevision = 0;
			MarkRenderStateDirty();
		}
	}

	auto DSkeletalMeshComponent::OnUnregister() -> void
	{
		AnimationInstance.Unbind();
		LastPublishedPoseRevision = 0;
		Super::OnUnregister();
	}

	auto DSkeletalMeshComponent::BeginPlay() -> void
	{
		Super::BeginPlay();
		std::string Error;
		if (!AnimationInstance.IsBound() && !RebindCurrent(Error))
		{
			DURIN_WARN("Skeletal-mesh component BeginPlay could not prepare playback. (component: {}, reason: {})",
				GetObjectPath(), Error);
			return;
		}
		if (bAutoPlay && AnimationClip && !AnimationInstance.Play(Error))
			DURIN_WARN("Skeletal-mesh component autoplay failed. (component: {}, reason: {})",
				GetObjectPath(), Error);
	}

	auto DSkeletalMeshComponent::TickComponent(float DeltaSeconds) -> void
	{
		Super::TickComponent(DeltaSeconds);
		if (!IsRegistered() || !HasBegunPlay() || IsBeingDestroyed()
			|| !IsComponentTickEnabled() || !AnimationInstance.IsBound()) return;
		std::string Error;
		if (!AnimationInstance.Tick(DeltaSeconds, Error))
			DURIN_WARN("Skeletal-mesh component playback tick failed. (component: {}, reason: {})",
				GetObjectPath(), Error);
		else PublishPoseDynamicData();
	}

	auto DSkeletalMeshComponent::EndPlay() -> void
	{
		AnimationInstance.Unbind();
		Super::EndPlay();
	}

	auto DSkeletalMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		if (!SkeletalMesh) return nullptr;
		const std::shared_ptr<const FSkeletalPosePalette> Pose = GetLatestPosePalette();
		if (!Pose || !Pose->LocalBounds.bIsValid) return nullptr;
		SkeletalMesh->InitResources();
		const FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetRenderData();
		if (!RenderData || RenderData->Sections.empty()) return nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		Materials.reserve(SkeletalMesh->GetMaterialSlots().size());
		for (uint32 SlotIndex = 0; SlotIndex < SkeletalMesh->GetNumMaterialSlots(); ++SlotIndex)
		{
			DMaterialInterface* Material = GetMaterial(SlotIndex);
			if (!Material) RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
			Materials.push_back(Material ? Material->GetMaterialRenderProxy()
				: GetDefaultMaterialRenderProxy());
		}
		LastPublishedPoseRevision = Pose->Revision;
		return std::make_unique<FSkeletalMeshSceneProxy>(
			RenderData, std::move(Materials), MaterialComponentRevision, Pose);
	}

#if DURIN_WITH_EDITOR
	auto DSkeletalMeshComponent::GetEditorPickingLocalBounds(
		FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool
	{
		const std::shared_ptr<const FSkeletalPosePalette> Pose = GetLatestPosePalette();
		if (!SkeletalMesh || !Pose) return false;
		OutBounds = Pose->LocalBounds;
		OutFamily = EEditorPickingPrimitiveFamily::SkeletalMesh;
		return OutBounds.bIsValid && Math::IsFinite(OutBounds.Min) && Math::IsFinite(OutBounds.Max);
	}
#endif

	auto DSkeletalMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		if (!SkeletalMesh || !SkeletalMesh->GetMaterialSlot(PendingMaterialSlotIndex)) return false;
		DMaterialInterface* Material = GetMaterial(PendingMaterialSlotIndex);
		if (!Material) RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.MaterialProxy = Material ? Material->GetMaterialRenderProxy()
			: GetDefaultMaterialRenderProxy();
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		return true;
	}
}

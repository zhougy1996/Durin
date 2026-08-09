#include "Components/SkeletalMeshComponent.h"

#include "DObject/DurinPropertyTypes.h"

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
		SetComponentTickEnabled(true);
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
		MarkPackageDirty();
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
		MarkPackageDirty();
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
		return AnimationInstance.Stop(OutError);
	}

	auto DSkeletalMeshComponent::Seek(float TimeSeconds, std::string& OutError) -> bool
	{
		return AnimationInstance.Seek(TimeSeconds, OutError);
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
		if (!std::isfinite(PlayRate) || PlayRate < 0.0f)
		{
			OutError = "Skeletal-mesh component play rate must be finite and non-negative.";
			return false;
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
	}

	auto DSkeletalMeshComponent::OnRegister() -> void
	{
		Super::OnRegister();
		std::string Error;
		if (!RebindCurrent(Error))
			DURIN_WARN("Skeletal-mesh component registration could not prepare playback. (component: {}, reason: {})",
				GetObjectPath(), Error);
	}

	auto DSkeletalMeshComponent::OnUnregister() -> void
	{
		AnimationInstance.Unbind();
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
	}

	auto DSkeletalMeshComponent::EndPlay() -> void
	{
		AnimationInstance.Unbind();
		Super::EndPlay();
	}

	auto DSkeletalMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		return nullptr;
	}
}

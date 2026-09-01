#include "Components/SkyBoxComponent.h"

#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "SceneInterface.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextSkyBoxInstanceId = 1;
	}

	DSkyBoxComponent::DSkyBoxComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, SkyBoxSceneId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose) ? FGuid{} : FGuid::NewGuid())
		, SkyBoxInstanceId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose)
			? 0
			: GNextSkyBoxInstanceId.fetch_add(1, std::memory_order_relaxed))
	{
	}

	auto DSkyBoxComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (!SkyBoxSceneId.IsValid()) SkyBoxSceneId = FGuid::NewGuid();
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::OnUnregister() -> void
	{
		if (FSceneInterface* Scene = GetRenderScene())
			Scene->RemoveSkyBox(FSkyBoxSceneId(SkyBoxInstanceId));
		Super::OnUnregister();
	}

	auto DSkyBoxComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Intensity")
			|| Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer) return true;
		float* DraftIntensity = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		*DraftIntensity = FMath::Max(0.0f, *DraftIntensity);
		return true;
	}

	auto DSkyBoxComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("TextureCube") || Name == FName("Tint") || Name == FName("Intensity"))
			MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::SetTextureCube(DTextureCube* InTextureCube) -> void
	{
		if (TextureCube.Get() == InTextureCube) return;
		TextureCube = InTextureCube;
		MarkPackageDirty();
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::SetTint(const FLinearColor& InTint) -> void
	{
		if (Tint == InTint) return;
		Tint = InTint;
		MarkPackageDirty();
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::SetIntensity(float InIntensity) -> void
	{
		const float NewIntensity = FMath::Max(0.0f, InIntensity);
		if (Intensity == NewIntensity) return;
		Intensity = NewIntensity;
		MarkPackageDirty();
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkSkyBoxRenderStateDirty();
	}

	auto DSkyBoxComponent::MarkSkyBoxRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		FSceneInterface* Scene = GetRenderScene();
		if (Scene == nullptr) return;

		if (const AActor* Owner = GetOwner(); Owner && Owner->IsHidden())
		{
			Scene->RemoveSkyBox(FSkyBoxSceneId(SkyBoxInstanceId));
			return;
		}

		FSkyBoxSceneData Data;
		Data.Rotation = GetWorldRotation();
		Data.Tint = FVector3f(Tint.R, Tint.G, Tint.B);
		Data.Intensity = FMath::Max(0.0f, Intensity);
		if (DTextureCube* Cube = TextureCube.Get())
			Data.TextureReference = Cube->GetTextureReferenceRHI();
		Scene->AddOrReplaceSkyBox(
			FSkyBoxSceneId(SkyBoxInstanceId),
			std::make_unique<FSkyBoxSceneProxy>(
				FSceneCandidateIdentity{SkyBoxSceneId, GetObjectPath()},
				std::move(Data)));
	}
}

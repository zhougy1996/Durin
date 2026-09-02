#include "Components/SkyBoxComponent.h"

#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "SceneInterface.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	DSkyBoxComponent::DSkyBoxComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DSkyBoxComponent::OnRegister() -> void
	{
		Super::OnRegister();
		CreateRenderState();
	}

	auto DSkyBoxComponent::OnUnregister() -> void
	{
		DestroyRenderState();
		Super::OnUnregister();
	}

	auto DSkyBoxComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Intensity")
			|| Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer) return true;
		float* DraftIntensity = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex
		);
		*DraftIntensity = FMath::Max(0.0f, *DraftIntensity);
		return true;
	}

	auto DSkyBoxComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("TextureCube") || Name == FName("Tint") || Name == FName("Intensity"))
			MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::SetTextureCube(DTextureCube* InTextureCube) -> void
	{
		if (TextureCube.Get() == InTextureCube) return;
		TextureCube = InTextureCube;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::SetTint(const FLinearColor& InTint) -> void
	{
		if (Tint == InTint) return;
		Tint = InTint;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::SetIntensity(float InIntensity) -> void
	{
		const float NewIntensity = FMath::Max(0.0f, InIntensity);
		if (Intensity == NewIntensity) return;
		Intensity = NewIntensity;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkRenderStateDirty();
	}

	auto DSkyBoxComponent::CreateRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene()) Scene->AddSkyBox(this);
	}

	auto DSkyBoxComponent::CreateSceneProxy() -> std::unique_ptr<FSkyBoxSceneProxy>
	{
		if (const AActor* Owner = GetOwner(); Owner && Owner->IsHidden()) return nullptr;
		FSkyBoxSceneData Data;
		Data.Rotation = GetWorldRotation();
		Data.Tint = FVector3f(Tint.R, Tint.G, Tint.B);
		Data.Intensity = FMath::Max(0.0f, Intensity);
		if (DTextureCube* Cube = TextureCube.Get())
			Data.TextureReference = Cube->GetTextureReferenceRHI();
		return std::make_unique<FSkyBoxSceneProxy>(std::move(Data));
	}

	auto DSkyBoxComponent::DestroyRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene()) Scene->RemoveSkyBox(this);
	}

	auto DSkyBoxComponent::MarkRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		DestroyRenderState();
		CreateRenderState();
	}
} // namespace Durin

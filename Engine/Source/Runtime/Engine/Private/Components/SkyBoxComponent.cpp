#include "Components/SkyBoxComponent.h"

#include "DObject/Property.h"
#include "Texture/TextureCube.h"
#include "Materials/MaterialInstance.h"
#include "Materials/DefaultMaterialService.h"

namespace Durin
{
	DSkyBoxComponent::DSkyBoxComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		bSelectable = false;
	}

	auto DSkyBoxComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (GetStaticMesh() == InStaticMesh) return;
		if (InStaticMesh) UpdateCubeMaterial();
		Super::SetStaticMesh(InStaticMesh);
	}

	auto DSkyBoxComponent::GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		if (SlotIndex == 0)
		{
			if (IsValid(MaterialOverride.Get())) return MaterialOverride.Get();
			if (CubeMaterial) return CubeMaterial.Get();
		}
		return Super::GetDefaultMaterial(SlotIndex);
	}

	auto DSkyBoxComponent::UpdateCubeMaterial() -> void
	{
		if (MaterialOverride) return;
		if (!CubeMaterial)
		{
			auto* Parent = GetDefaultCubeSkyMaterial();
			if (!Parent) return;
			CubeMaterial = DMaterialInstance::CreateDynamic(Parent, this);
		}
		if (!CubeMaterial) return;
		CubeMaterial->SetTextureParameterValue(FDefaultCubeSkyBindings::TextureName, GetTextureCube());
		CubeMaterial->SetVectorParameterValue(FDefaultCubeSkyBindings::TintName, FVector3(Tint.R, Tint.G, Tint.B));
		CubeMaterial->SetScalarParameterValue(FDefaultCubeSkyBindings::IntensityName, Intensity);
	}

	auto DSkyBoxComponent::RefreshAppearance(bool bRefreshMeshBinding) -> void
	{
		if (GetStaticMesh())
		{
			const bool bHadCubeMaterial = CubeMaterial != nullptr;
			UpdateCubeMaterial();
			if (bRefreshMeshBinding || (!bHadCubeMaterial && CubeMaterial)) MarkRenderStateDirty();
		}
	}

	auto DSkyBoxComponent::OnRegister() -> void
	{
		if (GetStaticMesh()) UpdateCubeMaterial();
		Super::OnRegister();
	}

	auto DSkyBoxComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Intensity")
			|| Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer) return {};
		float* DraftIntensity = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex
		);
		*DraftIntensity = FMath::Max(0.0f, *DraftIntensity);
		return {};
	}

	auto DSkyBoxComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("TextureCube") || Name == FName("Tint") || Name == FName("Intensity")
			|| Name == FName("MaterialOverride") || Name == FName("StaticMesh"))
			RefreshAppearance(Name == FName("MaterialOverride") || Name == FName("StaticMesh"));
	}

	auto DSkyBoxComponent::GetTextureCube() const -> DTextureCube*
	{
		auto* Cube = TextureCube.Get();
		return IsValid(Cube) ? Cube : nullptr;
	}

	auto DSkyBoxComponent::SetTextureCube(DTextureCube* InTextureCube) -> void
	{
		if (TextureCube.Get() == InTextureCube) return;
		TextureCube = InTextureCube;
		MarkPackageDirty();
		RefreshAppearance();
	}

	auto DSkyBoxComponent::SetMaterialOverride(DMaterialInterface* Material) -> void
	{
		if (MaterialOverride.Get() == Material) return;
		MaterialOverride = Material;
		MarkPackageDirty();
		RefreshAppearance(true);
	}

	auto DSkyBoxComponent::SetTint(const FLinearColor& InTint) -> void
	{
		if (Tint == InTint) return;
		Tint = InTint;
		MarkPackageDirty();
		RefreshAppearance();
	}

	auto DSkyBoxComponent::SetIntensity(float InIntensity) -> void
	{
		const float NewIntensity = FMath::Max(0.0f, InIntensity);
		if (Intensity == NewIntensity) return;
		Intensity = NewIntensity;
		MarkPackageDirty();
		RefreshAppearance();
	}
} // namespace Durin

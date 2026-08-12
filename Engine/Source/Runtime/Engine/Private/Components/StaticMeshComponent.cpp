#include "Components/StaticMeshComponent.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/Level.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "Materials/DefaultMaterialService.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshMaterialBinding.h"

namespace Durin
{
	DStaticMeshComponent::DStaticMeshComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		StaticMesh = InStaticMesh;
		++MaterialComponentRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::GetStaticMesh() const -> DStaticMesh*
	{
		return StaticMesh.Get();
	}

	auto DStaticMeshComponent::GetBodySetup() const -> DBodySetup*
	{
		return StaticMesh ? StaticMesh->EnsureQualifiedBoxBodySetup() : nullptr;
	}

	auto DStaticMeshComponent::BuildCollisionShape(
		FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool
	{
		DBodySetup* Setup = GetBodySetup();
		FTransform LocalTransform;
		if (!Setup || !Setup->BuildShape(OutShape, LocalTransform)) return false;
		OutWorldTransform = FTransform::Combine(GetWorldTransform(), LocalTransform);
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DStaticMeshComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		DBodySetup* Setup = GetBodySetup();
		FTransform LocalTransform;
		if (!Setup || !Setup->BuildGeometry(OutGeometry, LocalTransform)) return false;
		OutWorldTransform = FTransform::Combine(GetWorldTransform(), LocalTransform);
		return IsValidPhysicsTransform(OutWorldTransform);
	}

	auto DStaticMeshComponent::GetCollisionStateRevision() const -> uint64
	{
		const DBodySetup* Setup = GetBodySetup();
		return Setup ? Setup->GetRevision() : 0;
	}

	auto DStaticMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> bool
	{
		return SetMaterial(0, InMaterial);
	}

	auto DStaticMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool
	{
		if (StaticMesh == nullptr || StaticMesh->GetMaterialSlot(SlotIndex) == nullptr) return false;
		if (InMaterial == nullptr)
		{
			if (!HasMaterialOverride(SlotIndex)) return true;
			return ResetMaterial(SlotIndex);
		}
		if (SlotIndex >= OverrideMaterials.size()) OverrideMaterials.resize(static_cast<size_t>(SlotIndex) + 1);
		if (OverrideMaterials[SlotIndex] == InMaterial) return true;
		OverrideMaterials[SlotIndex] = InMaterial;
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DStaticMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (Slot == nullptr) return nullptr;
		if (DMaterialInterface* Override = GetMaterialOverride(SlotIndex)) return Override;
		return Slot->DefaultMaterial.Get();
	}

	auto DStaticMeshComponent::SetMaterialByName(FName SlotName, DMaterialInterface* InMaterial) -> bool
	{
		if (StaticMesh == nullptr) return false;
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh->FindMaterialSlot(SlotName);
		if (Slot == nullptr) return false;
		return SetMaterial(static_cast<uint32>(Slot - StaticMesh->GetMaterialSlots().data()), InMaterial);
	}

	auto DStaticMeshComponent::GetMaterialByName(FName SlotName) const -> DMaterialInterface*
	{
		if (StaticMesh == nullptr) return nullptr;
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh->FindMaterialSlot(SlotName);
		return Slot != nullptr
			? GetMaterial(static_cast<uint32>(Slot - StaticMesh->GetMaterialSlots().data()))
			: nullptr;
	}

	auto DStaticMeshComponent::ResetMaterial(uint32 SlotIndex) -> bool
	{
		if (StaticMesh == nullptr || StaticMesh->GetMaterialSlot(SlotIndex) == nullptr
			|| !HasMaterialOverride(SlotIndex)) return false;
		OverrideMaterials[SlotIndex] = nullptr;
		TrimTrailingNullOverrides();
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = SlotIndex;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DStaticMeshComponent::ClearMaterialOverrides() -> bool
	{
		if (OverrideMaterials.empty()) return false;
		OverrideMaterials.clear();
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = 0;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DStaticMeshComponent::GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*
	{
		return SlotIndex < OverrideMaterials.size() ? OverrideMaterials[SlotIndex].Get() : nullptr;
	}

	auto DStaticMeshComponent::HasMaterialOverride(uint32 SlotIndex) const -> bool
	{
		return GetMaterialOverride(SlotIndex) != nullptr;
	}

	auto DStaticMeshComponent::TrimTrailingNullOverrides() -> void
	{
		TrimTrailingNullStaticMeshMaterialOverrides(OverrideMaterials);
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh != nullptr ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DStaticMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (!ValidateOverrideMaterials(OverrideMaterials, OutError)) return false;
		TrimTrailingNullOverrides();
		return true;
	}

	auto DStaticMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			{
				OutError = "The static-mesh object property metadata is unavailable.";
				return false;
			}
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Value && !Cast<DStaticMesh>(Value))
			{
				OutError = "Selected asset is not a static mesh.";
				return false;
			}
			return true;
		}
		if (Name != FName("OverrideMaterials")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			OutError = "The static-mesh material array metadata is unavailable.";
			return false;
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		if (!ArrayProperty->GetInner() || ArrayProperty->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			OutError = "The static-mesh material override metadata is unavailable.";
			return false;
		}
		std::vector<TObjectPtr<DMaterialInterface>> Overrides;
		Overrides.reserve(static_cast<size_t>(ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex)));
		for (uint64 Index = 0; Index < ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex); ++Index)
		{
			DObject* Element = static_cast<const FObjectProperty*>(ArrayProperty->GetInner())->GetObjectPropertyValue(
				ArrayProperty->GetElementPtr(Proposal.DraftRootContainer, Index, Proposal.DraftRootArrayIndex));
			DMaterialInterface* Material = Cast<DMaterialInterface>(Element);
			if (Element != nullptr && Material == nullptr)
			{
				OutError = std::format(
					"A static-mesh component contains an incompatible object at material index {}.", Index);
				return false;
			}
			Overrides.push_back(Material);
		}
		return ValidateOverrideMaterials(Overrides, OutError);
	}

	auto DStaticMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			++MaterialComponentRevision;
			MarkRenderStateDirty();
			RecreatePhysicsState();
			return;
		}
		if (Name != FName("OverrideMaterials")) return;
		TrimTrailingNullOverrides();
		++MaterialComponentRevision;
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		StaticMesh->InitResources();
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr
			|| RenderData->LODResources.empty()
			|| RenderData->LODResources[0].GetNumIndices() == 0)
		{
			return nullptr;
		}

		std::vector<FMaterialRenderProxyRef> MaterialProxies;
		MaterialProxies.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			if (SlotMaterial == nullptr)
			{
				RecordMaterialFallbackReason(
					EMaterialFallbackReason::UnassignedDefault);
			}
			MaterialProxies.push_back(
				SlotMaterial != nullptr
					? SlotMaterial->GetMaterialRenderProxy()
					: GetDefaultMaterialRenderProxy());
		}
		return std::make_unique<FStaticMeshSceneProxy>(
			RenderData,
			std::move(MaterialProxies),
			MaterialComponentRevision);
	}

#if DURIN_WITH_EDITOR
	auto DStaticMeshComponent::GetEditorPickingLocalBounds(
		FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool
	{
		const FStaticMeshRenderData* Data = StaticMesh ? StaticMesh->GetRenderData() : nullptr;
		if (!Data || Data->LODResources.empty()) return false;
		OutBounds = Data->LODResources[0].LocalBounds;
		OutFamily = EEditorPickingPrimitiveFamily::StaticMesh;
		return OutBounds.bIsValid && Math::IsFinite(OutBounds.Min) && Math::IsFinite(OutBounds.Max);
	}
#endif

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get()) return;
		++MaterialComponentRevision;
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DStaticMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* CurrentMaterial = GetMaterial(PendingMaterialSlotIndex);
		if (CurrentMaterial == nullptr)
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::UnassignedDefault);
		}
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.MaterialProxy = CurrentMaterial != nullptr
			? CurrentMaterial->GetMaterialRenderProxy()
			: GetDefaultMaterialRenderProxy();
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		return true;
	}

	auto DStaticMeshComponent::ValidateOverrideMaterials(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		std::string& OutError) const -> bool
	{
		return ValidateStaticMeshMaterialOverrides(Overrides, "static-mesh component", OutError);
	}

}

#include "Components/StaticMeshComponent.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		auto FindOverride(std::span<const FStaticMeshMaterialOverride> Overrides, const FGuid& SlotId)
			-> const FStaticMeshMaterialOverride*
		{
			const auto It = std::ranges::find(Overrides, SlotId, &FStaticMeshMaterialOverride::SlotId);
			return It == Overrides.end() ? nullptr : &*It;
		}
	}

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
	}

	auto DStaticMeshComponent::GetStaticMesh() const -> DStaticMesh*
	{
		return StaticMesh.Get();
	}

	auto DStaticMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> void
	{
		SetMaterial(0, InMaterial);
	}

	auto DStaticMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> void
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		if (Slot != nullptr) SetMaterialBySlotId(Slot->SlotId, InMaterial);
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DStaticMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->GetMaterialSlot(SlotIndex) : nullptr;
		return Slot != nullptr ? GetMaterialBySlotId(Slot->SlotId) : nullptr;
	}

	auto DStaticMeshComponent::SetMaterialBySlotId(const FGuid& SlotId, DMaterialInterface* InMaterial) -> bool
	{
		const FStaticMeshMaterialSlotDefinition* Slot =
			StaticMesh != nullptr ? StaticMesh->FindMaterialSlot(SlotId) : nullptr;
		if (!SlotId.IsValid() || Slot == nullptr) return false;
		if (InMaterial == nullptr) return ResetMaterialBySlotId(SlotId);
		if (FStaticMeshMaterialOverride* Override = const_cast<FStaticMeshMaterialOverride*>(FindOverride(MaterialOverrides, SlotId)))
		{
			if (Override->Material == InMaterial) return true;
			Override->Material = InMaterial;
		}
		else MaterialOverrides.push_back({.SlotId = SlotId, .Material = InMaterial});
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = static_cast<uint32>(
			Slot - StaticMesh->GetMaterialSlots().data());
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DStaticMeshComponent::ResetMaterialBySlotId(const FGuid& SlotId) -> bool
	{
		if (!SlotId.IsValid() || StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr) return false;
		return RemoveMaterialOverride(SlotId);
	}

	auto DStaticMeshComponent::RemoveMaterialOverride(const FGuid& SlotId) -> bool
	{
		const FStaticMeshMaterialSlotDefinition* Slot =
			StaticMesh != nullptr ? StaticMesh->FindMaterialSlot(SlotId) : nullptr;
		const size_t PreviousSize = MaterialOverrides.size();
		std::erase_if(MaterialOverrides, [&SlotId](const FStaticMeshMaterialOverride& Override) { return Override.SlotId == SlotId; });
		if (MaterialOverrides.size() == PreviousSize) return false;
		++MaterialComponentRevision;
		PendingMaterialSlotIndex = Slot != nullptr
			? static_cast<uint32>(Slot - StaticMesh->GetMaterialSlots().data())
			: 0;
		MarkPackageDirty();
		if (Slot != nullptr)
		{
			MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		}
		return true;
	}

	auto DStaticMeshComponent::GetMaterialBySlotId(const FGuid& SlotId) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialSlotDefinition* Slot = StaticMesh != nullptr ? StaticMesh->FindMaterialSlot(SlotId) : nullptr;
		if (Slot == nullptr) return nullptr;
		if (const FStaticMeshMaterialOverride* Override = FindOverride(MaterialOverrides, SlotId)) return Override->Material.Get();
		return Slot->DefaultMaterial.Get();
	}

	auto DStaticMeshComponent::GetMaterialOverride(const FGuid& SlotId) const -> DMaterialInterface*
	{
		const FStaticMeshMaterialOverride* Override = FindOverride(MaterialOverrides, SlotId);
		return Override != nullptr ? Override->Material.Get() : nullptr;
	}

	auto DStaticMeshComponent::HasMaterialOverride(const FGuid& SlotId) const -> bool
	{
		return FindOverride(MaterialOverrides, SlotId) != nullptr;
	}

	auto DStaticMeshComponent::IsMaterialOverrideOrphan(const FGuid& SlotId) const -> bool
	{
		return HasMaterialOverride(SlotId) && (StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr);
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		return StaticMesh != nullptr ? StaticMesh->GetNumMaterialSlots() : 0;
	}

	auto DStaticMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		return ValidateMaterialOverrides(MaterialOverrides, OutError);
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
		if (Name != FName("MaterialOverrides")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			OutError = "The static-mesh material array metadata is unavailable.";
			return false;
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		if (!ArrayProperty->GetInner() || ArrayProperty->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct)
		{
			OutError = "The static-mesh material override metadata is unavailable.";
			return false;
		}
		std::vector<FStaticMeshMaterialOverride> Overrides;
		Overrides.reserve(static_cast<size_t>(ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex)));
		for (uint64 Index = 0; Index < ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex); ++Index)
		{
			const auto* Element = static_cast<const FStaticMeshMaterialOverride*>(
				ArrayProperty->GetElementPtr(Proposal.DraftRootContainer, Index, Proposal.DraftRootArrayIndex));
			if (Element != nullptr) Overrides.push_back(*Element);
		}
		return ValidateMaterialOverrides(Overrides, OutError);
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
			return;
		}
		if (Name != FName("MaterialOverrides")) return;
		++MaterialComponentRevision;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
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
			MaterialProxies.push_back(
				SlotMaterial != nullptr
					? SlotMaterial->GetMaterialRenderProxy()
					: FMaterialRenderProxyRef{});
		}
		return std::make_unique<FStaticMeshSceneProxy>(
			RenderData,
			std::move(MaterialProxies),
			MaterialComponentRevision);
	}

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get()) return;
		++MaterialComponentRevision;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* CurrentMaterial = GetMaterial(PendingMaterialSlotIndex);
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.MaterialProxy = CurrentMaterial != nullptr
			? CurrentMaterial->GetMaterialRenderProxy()
			: FMaterialRenderProxyRef{};
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		return true;
	}

	auto DStaticMeshComponent::ValidateMaterialOverrides(
		std::span<const FStaticMeshMaterialOverride> Overrides,
		std::string& OutError) const -> bool
	{
		std::unordered_set<FGuid> SlotIds;
		for (const FStaticMeshMaterialOverride& Override : Overrides)
		{
			if (!Override.SlotId.IsValid())
			{
				OutError = "A static-mesh component contains a material override with an invalid slot GUID.";
				return false;
			}
			if (!SlotIds.insert(Override.SlotId).second)
			{
				OutError = std::format("A static-mesh component contains duplicate overrides for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
			if (Override.Material == nullptr)
			{
				OutError = std::format("A static-mesh component contains a null override for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
			DObject* MaterialObject = reinterpret_cast<DObject*>(Override.Material.Get());
			if (Cast<DMaterialInterface>(MaterialObject) == nullptr)
			{
				OutError = std::format("A static-mesh component contains an incompatible object for slot GUID {}.", Override.SlotId.ToString());
				return false;
			}
		}
		return true;
	}

}

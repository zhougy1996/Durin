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

	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		StaticMesh = InStaticMesh;
		ReconcileStaticMeshBinding();
		ReconcileMaterialBindings();
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
		if (!SlotId.IsValid() || StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr) return false;
		if (InMaterial == nullptr) return ResetMaterialBySlotId(SlotId);
		if (FStaticMeshMaterialOverride* Override = const_cast<FStaticMeshMaterialOverride*>(FindOverride(MaterialOverrides, SlotId)))
		{
			if (Override->Material == InMaterial) return true;
			Override->Material = InMaterial;
		}
		else MaterialOverrides.push_back({.SlotId = SlotId, .Material = InMaterial});
		ReconcileMaterialBindings();
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DStaticMeshComponent::ResetMaterialBySlotId(const FGuid& SlotId) -> bool
	{
		if (!SlotId.IsValid() || StaticMesh == nullptr || StaticMesh->FindMaterialSlot(SlotId) == nullptr) return false;
		return RemoveMaterialOverride(SlotId);
	}

	auto DStaticMeshComponent::RemoveMaterialOverride(const FGuid& SlotId) -> bool
	{
		const size_t PreviousSize = MaterialOverrides.size();
		std::erase_if(MaterialOverrides, [&SlotId](const FStaticMeshMaterialOverride& Override) { return Override.SlotId == SlotId; });
		if (MaterialOverrides.size() == PreviousSize) return false;
		ReconcileMaterialBindings();
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkPackageDirty();
		if (StaticMesh != nullptr && StaticMesh->FindMaterialSlot(SlotId) != nullptr) MarkRenderStateDirty();
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
		if (!ValidateMaterialOverrides(MaterialOverrides, OutError)) return false;
		ReconcileStaticMeshBinding();
		ReconcileMaterialBindings();
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
			ReconcileStaticMeshBinding();
			ReconcileMaterialBindings();
			++MaterialComponentRevision;
			MarkRenderStateDirty();
			return;
		}
		if (Name != FName("MaterialOverrides")) return;
		ReconcileMaterialBindings();
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr || RenderData->LODResources.empty() || RenderData->LODResources[0].Indices.empty())
		{
			return nullptr;
		}

		std::vector<FMaterialRenderUpdate> MaterialUpdates;
		MaterialUpdates.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			FMaterialRenderUpdate& Update = MaterialUpdates.emplace_back();
			Update.SlotIndex = SlotIndex;
			Update.RenderData = SlotMaterial != nullptr ? SlotMaterial->GetRenderData() : FMaterialRenderData{};
			Update.MaterialVersion = SlotMaterial != nullptr ? SlotMaterial->GetRenderStateVersion() : 0;
			Update.ComponentRevision = MaterialComponentRevision;
			Update.DirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		}
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;
		return std::make_unique<FStaticMeshSceneProxy>(RenderData, std::move(MaterialUpdates));
	}

	auto DStaticMeshComponent::BeginDestroy() -> void
	{
		if (BoundStaticMesh != nullptr) BoundStaticMesh->RemoveBoundComponent(this);
		BoundStaticMesh = nullptr;
		for (const TObjectPtr<DMaterialInterface>& SlotMaterial : BoundMaterials) UnbindMaterial(SlotMaterial.Get());
		BoundMaterials.clear();
		Super::BeginDestroy();
	}

	auto DStaticMeshComponent::HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void
	{
		if (ChangedMesh == nullptr || ChangedMesh != StaticMesh.Get() || ChangedMesh != BoundStaticMesh) return;
		ReconcileMaterialBindings();
		++MaterialComponentRevision;
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* CurrentMaterial = GetMaterial(PendingMaterialSlotIndex);
		OutUpdate.SlotIndex = PendingMaterialSlotIndex;
		OutUpdate.RenderData = CurrentMaterial != nullptr ? CurrentMaterial->GetRenderData() : FMaterialRenderData{};
		OutUpdate.MaterialVersion = CurrentMaterial != nullptr ? CurrentMaterial->GetRenderStateVersion() : 0;
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		OutUpdate.DirtyFlags = PendingMaterialDirtyFlags != EMaterialRenderDirtyFlags::None ? PendingMaterialDirtyFlags : DirtyFlags;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;
		return true;
	}

	auto DStaticMeshComponent::HandleMaterialRenderDataChanged(DMaterialInterface* ChangedMaterial, EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		const uint32 SlotCount = GetNumMaterials();
		for (uint32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
		{
			if (GetMaterial(SlotIndex) != ChangedMaterial) continue;
			// Component revisions order updates even when different slots refer to assets with unrelated versions.
			++MaterialComponentRevision;
			PendingMaterialSlotIndex = SlotIndex;
			PendingMaterialDirtyFlags = DirtyFlags;
			MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialData);
		}
	}

	auto DStaticMeshComponent::BindMaterial(DMaterialInterface* InMaterial) -> void
	{
		if (InMaterial != nullptr) InMaterial->AddBoundComponent(this);
	}

	auto DStaticMeshComponent::UnbindMaterial(DMaterialInterface* InMaterial) -> void
	{
		if (InMaterial != nullptr) InMaterial->RemoveBoundComponent(this);
	}

	auto DStaticMeshComponent::ReconcileStaticMeshBinding() -> void
	{
		DStaticMesh* CurrentMesh = StaticMesh.Get();
		if (BoundStaticMesh == CurrentMesh) return;
		if (BoundStaticMesh != nullptr) BoundStaticMesh->RemoveBoundComponent(this);
		BoundStaticMesh = CurrentMesh;
		if (BoundStaticMesh != nullptr) BoundStaticMesh->AddBoundComponent(this);
	}

	auto DStaticMeshComponent::ReconcileMaterialBindings() -> void
	{
		std::vector<TObjectPtr<DMaterialInterface>> ResolvedMaterials;
		if (StaticMesh != nullptr)
		{
			for (const FStaticMeshMaterialSlotDefinition& Slot : StaticMesh->GetMaterialSlots())
			{
				DMaterialInterface* Resolved = GetMaterialBySlotId(Slot.SlotId);
				if (Resolved != nullptr && std::ranges::none_of(ResolvedMaterials,
					[Resolved](const TObjectPtr<DMaterialInterface>& Candidate) { return Candidate.Get() == Resolved; }))
					ResolvedMaterials.push_back(Resolved);
			}
		}
		for (const TObjectPtr<DMaterialInterface>& Previous : BoundMaterials)
		{
			if (Previous != nullptr && std::ranges::none_of(ResolvedMaterials, [&](const auto& Current) { return Current == Previous; }))
				UnbindMaterial(Previous.Get());
		}
		for (const TObjectPtr<DMaterialInterface>& Current : ResolvedMaterials)
		{
			if (Current != nullptr && std::ranges::none_of(BoundMaterials, [&](const auto& Previous) { return Previous == Current; }))
				BindMaterial(Current.Get());
		}
		BoundMaterials = std::move(ResolvedMaterials);
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

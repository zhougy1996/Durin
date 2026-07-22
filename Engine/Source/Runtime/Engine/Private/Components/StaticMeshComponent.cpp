#include "Components/StaticMeshComponent.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		StaticMesh = InStaticMesh;
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
		if (GetMaterial(SlotIndex) == InMaterial) return;
		if (Materials.size() <= SlotIndex) Materials.resize(static_cast<size_t>(SlotIndex) + 1);
		Materials[SlotIndex] = InMaterial;
		if (SlotIndex == 0) Material = InMaterial;
		ReconcileMaterialBindings();
		++MaterialComponentRevision;
		PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DStaticMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		if (SlotIndex < Materials.size()) return Materials[SlotIndex].Get();
		return SlotIndex == 0 ? Material.Get() : nullptr;
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		const FStaticMeshRenderData* RenderData = StaticMesh != nullptr ? StaticMesh->GetRenderData() : nullptr;
		return RenderData != nullptr ? static_cast<uint32>(RenderData->MaterialSlots.size()) : 0;
	}

	auto DStaticMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (Materials.empty() && Material != nullptr) Materials.push_back(Material);
		if (!Materials.empty()) Material = Materials[0];
		ReconcileMaterialBindings();
		return true;
	}

	auto DStaticMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh") || Name == FName("Material"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			{
				OutError = "The static-mesh object property metadata is unavailable.";
				return false;
			}
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Name == FName("StaticMesh") && Value && !Cast<DStaticMesh>(Value))
			{
				OutError = "Selected asset is not a static mesh.";
				return false;
			}
			if (Name == FName("Material") && Value && !Cast<DMaterialInterface>(Value))
			{
				OutError = "Selected asset is not a material.";
				return false;
			}
			return true;
		}
		if (Name != FName("Materials")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			OutError = "The static-mesh material array metadata is unavailable.";
			return false;
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		auto* ObjectProperty = ArrayProperty->GetInner() && ArrayProperty->GetInner()->GetKind() == DurinCodeGen::EPropertyGenFlags::Object
			? static_cast<const FObjectProperty*>(ArrayProperty->GetInner()) : nullptr;
		if (!ObjectProperty)
		{
			OutError = "The static-mesh material array metadata is unavailable.";
			return false;
		}
		for (uint64 Index = 0; Index < ArrayProperty->Num(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex); ++Index)
		{
			const void* Element = ArrayProperty->GetElementPtr(Proposal.DraftRootContainer, Index, Proposal.DraftRootArrayIndex);
			DObject* Value = Element ? ObjectProperty->GetObjectPropertyValue(Element) : nullptr;
			if (Value && !Cast<DMaterialInterface>(Value))
			{
				OutError = "Selected asset is not a material.";
				return false;
			}
		}
		return true;
	}

	auto DStaticMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticMesh"))
		{
			MarkRenderStateDirty();
			return;
		}
		if (Name == FName("Material"))
		{
			if (Materials.empty()) Materials.resize(1);
			Materials[0] = Material;
		}
		else if (Name == FName("Materials"))
		{
			Material = Materials.empty() ? nullptr : Materials[0];
		}
		else return;
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
		for (const TObjectPtr<DMaterialInterface>& SlotMaterial : BoundMaterials) UnbindMaterial(SlotMaterial.Get());
		BoundMaterials.clear();
		Super::BeginDestroy();
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
		const uint32 SlotCount = std::max<uint32>(static_cast<uint32>(Materials.size()), 1);
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

	auto DStaticMeshComponent::ReconcileMaterialBindings() -> void
	{
		for (const TObjectPtr<DMaterialInterface>& Previous : BoundMaterials)
		{
			if (Previous != nullptr && std::ranges::none_of(Materials, [&](const auto& Current) { return Current == Previous; }))
				UnbindMaterial(Previous.Get());
		}
		for (const TObjectPtr<DMaterialInterface>& Current : Materials)
		{
			if (Current != nullptr && std::ranges::none_of(BoundMaterials, [&](const auto& Previous) { return Previous == Current; }))
				BindMaterial(Current.Get());
		}
		BoundMaterials = Materials;
	}
}

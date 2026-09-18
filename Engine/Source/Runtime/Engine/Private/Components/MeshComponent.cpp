#include "Components/MeshComponent.h"

#include "Components/ComponentMaterialOverride.h"
#include "Asset/Load.h"
#include "DObject/DurinPropertyTypes.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MeshMaterialSlot.h"

namespace Durin
{
	auto DMeshComponent::GetNumMaterials() const -> uint32
	{
		return 0;
	}

	auto DMeshComponent::GetMaterialIndex(FName) const -> std::optional<uint32>
	{
		return std::nullopt;
	}

	auto DMeshComponent::GetDefaultMaterial(uint32) const -> DMaterialInterface*
	{
		return nullptr;
	}

	auto DMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> bool
	{
		return SetMaterial(0, InMaterial);
	}

	auto DMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool
	{
		const auto Result = ComponentMaterialOverride::Set(
			OverrideMaterials, SlotIndex, SlotIndex < GetNumMaterials(),
			InMaterial, MaterialComponentRevision, PendingMaterialSlotIndex);
		if (Result == ComponentMaterialOverride::EMutationResult::InvalidSlot) return false;
		if (Result == ComponentMaterialOverride::EMutationResult::Unchanged) return true;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		if (SlotIndex >= GetNumMaterials()) return nullptr;
		return ComponentMaterialOverride::Resolve(OverrideMaterials, SlotIndex, GetDefaultMaterial(SlotIndex));
	}

	auto DMeshComponent::SetMaterialByName(FName SlotName, DMaterialInterface* InMaterial) -> bool
	{
		const auto Index = GetMaterialIndex(SlotName);
		return Index && SetMaterial(*Index, InMaterial);
	}

	auto DMeshComponent::GetMaterialByName(FName SlotName) const -> DMaterialInterface*
	{
		const auto Index = GetMaterialIndex(SlotName);
		return Index ? GetMaterial(*Index) : nullptr;
	}

	auto DMeshComponent::ResetMaterial(uint32 SlotIndex) -> bool
	{
		const auto Result = ComponentMaterialOverride::Set(
			OverrideMaterials, SlotIndex, SlotIndex < GetNumMaterials(),
			nullptr, MaterialComponentRevision, PendingMaterialSlotIndex);
		if (Result != ComponentMaterialOverride::EMutationResult::Changed) return false;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		return true;
	}

	auto DMeshComponent::ClearMaterialOverrides() -> bool
	{
		if (!ComponentMaterialOverride::Clear(
			OverrideMaterials, MaterialComponentRevision, PendingMaterialSlotIndex)) return false;
		MarkPackageDirty();
		MarkRenderStateDirty();
		return true;
	}

	auto DMeshComponent::GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*
	{
		return ComponentMaterialOverride::Get(OverrideMaterials, SlotIndex);
	}

	auto DMeshComponent::HasMaterialOverride(uint32 SlotIndex) const -> bool
	{
		return GetMaterialOverride(SlotIndex) != nullptr;
	}

	auto DMeshComponent::MigrateMaterialOverrides(std::vector<TObjectPtr<DMaterialInterface>>& LegacyOverrides) -> void
	{
		if (OverrideMaterials.empty()) OverrideMaterials = std::move(LegacyOverrides);
		LegacyOverrides.clear();
		ReportAssetLoadMutation(this, "Engine.MeshComponent.MaterialOverrides",
			"Component material overrides were moved to MeshComponent.", EAssetLoadMutationKind::Upgrade);
		MarkPackageDirty();
	}

	auto DMeshComponent::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
		if (!ValidateOverrideMaterials(OverrideMaterials, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}; clearing material overrides.", GetObjectPath(), Error);
			OverrideMaterials.clear();
		}
		ComponentMaterialOverride::TrimTrailingNulls(OverrideMaterials);
	}

	auto DMeshComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		ComponentMaterialOverride::BuildRenderProxyBindingUpdate(
			PendingMaterialSlotIndex, GetMaterial(PendingMaterialSlotIndex),
			MaterialComponentRevision, OutUpdate);
		return true;
	}

	auto DMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name != FName("OverrideMaterials")) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			OutError = "The mesh material array metadata is unavailable.";
			return false;
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		if (!ArrayProperty->GetInner() || ArrayProperty->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			OutError = "The mesh material override metadata is unavailable.";
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
					"A mesh component contains an incompatible object at material index {}.", Index);
				return false;
			}
			Overrides.push_back(Material);
		}
		return ValidateOverrideMaterials(Overrides, OutError);
	}

	auto DMeshComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name != FName("OverrideMaterials")) return;
		ComponentMaterialOverride::TrimTrailingNulls(OverrideMaterials);
		++MaterialComponentRevision;
		MarkRenderStateDirty();
	}

	auto DMeshComponent::ValidateOverrideMaterials(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		std::string& OutError) const -> bool
	{
		if (Overrides.size() > MaximumMeshMaterialSlots)
		{
			OutError = std::format("A mesh component contains {} positional material entries, exceeding the limit of {}.",
				Overrides.size(), MaximumMeshMaterialSlots);
			return false;
		}
		for (size_t Index = 0; Index < Overrides.size(); ++Index)
		{
			if (Overrides[Index]
				&& !Cast<DMaterialInterface>(reinterpret_cast<DObject*>(Overrides[Index].Get())))
			{
				OutError = std::format("A mesh component contains an incompatible object at material index {}.", Index);
				return false;
			}
		}
		return true;
	}
}

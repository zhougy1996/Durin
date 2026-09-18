#include "Components/MeshComponent.h"
#include "Components/PropertyEditValidation.h"

#include "Components/ComponentMaterialOverride.h"
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

	auto DMeshComponent::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> FObjectValidationResult
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (const auto Validation = ValidateOverrideMaterials(OverrideMaterials); !Validation)
			return RejectLoadedObjectGraph(GetObjectPath(), FormatStaticMeshMaterialOverrideError(Validation.Error, "mesh component"));
		return {};
	}

	auto DMeshComponent::PostLoad() -> void
	{
		Super::PostLoad();
		if (const auto Validation = ValidateOverrideMaterials(OverrideMaterials); !Validation)
		{
			DURIN_ERROR("PostLoad '{}': {}; clearing material overrides.", GetObjectPath(), FormatStaticMeshMaterialOverrideError(Validation.Error, "mesh component"));
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

	auto DMeshComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> FObjectValidationResult
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name != FName("OverrideMaterials")) return {};
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
		}
		auto* ArrayProperty = static_cast<const FArrayProperty*>(Proposal.DraftRootProperty);
		if (!ArrayProperty->GetInner() || ArrayProperty->GetInner()->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
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
				return RejectEnginePropertyEdit(*this, Proposal, FStaticMeshMaterialOverrideError{
					.Code = EStaticMeshMaterialOverrideError::IncompatibleObject, .Index = Index,
					.ObjectPath = Element->GetObjectPath(), .ActualType = Element->GetClass()->GetQualifiedName().ToString()});
			}
			Overrides.push_back(Material);
		}
		const auto Validation = ValidateOverrideMaterials(Overrides);
		if (!Validation) return RejectEnginePropertyEdit(*this, Proposal, Validation.Error);
		return {};
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
		std::span<const TObjectPtr<DMaterialInterface>> Overrides) const -> FStaticMeshMaterialOverrideResult
	{
		return ValidateStaticMeshMaterialOverrides(Overrides);
	}
}

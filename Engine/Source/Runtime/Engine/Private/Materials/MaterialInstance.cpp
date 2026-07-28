#include "Materials/MaterialInstance.h"

#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto FindMutableOverride(
			std::vector<FMaterialParameterOverride>& Overrides,
			const FGuid& Id
		) -> FMaterialParameterOverride*
		{
			const auto It = std::ranges::find(Overrides, Id, &FMaterialParameterOverride::ParameterId);
			return It == Overrides.end() ? nullptr : &*It;
		}

		auto FindOverride(
			const std::vector<FMaterialParameterOverride>& Overrides,
			const FGuid& Id
		) -> const FMaterialParameterOverride*
		{
			const auto It = std::ranges::find(Overrides, Id, &FMaterialParameterOverride::ParameterId);
			return It == Overrides.end() ? nullptr : &*It;
		}

		auto CanonicalizeParameterValue(
			EMaterialParameterType Type,
			const FMaterialParameterValue& Value
		) -> FMaterialParameterValue
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar:
				return FMaterialParameterValue::MakeScalar(Value.ScalarValue);
			case EMaterialParameterType::Vector:
				return FMaterialParameterValue::MakeVector(Value.VectorValue);
			case EMaterialParameterType::Texture:
				return FMaterialParameterValue::MakeTexture(Value.TextureValue.Get());
			}
			return {};
		}

		auto WouldCreateParentCycle(
			const DMaterialInstance* Instance,
			const DMaterialInterface* CandidateParent
		) -> bool
		{
			std::unordered_set<const DMaterialInterface*> Visited;
			for (const DMaterialInterface* Candidate = CandidateParent;
				Candidate != nullptr;
				Candidate = Candidate->GetParent())
			{
				if (Candidate == Instance || !Visited.insert(Candidate).second) return true;
			}
			return false;
		}
	}

	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		if (WouldCreateParentCycle(this, InParent)) return false;
		if (Parent == InParent) return true;
		Parent = InParent;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Parent")
			|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			OutError = "The material parent metadata is unavailable.";
			return false;
		}
		DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		auto* CandidateParent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
		if (Value && !CandidateParent)
		{
			OutError = "Selected asset is not a material.";
			return false;
		}
		if (WouldCreateParentCycle(this, CandidateParent))
		{
			OutError = "A material instance cannot create a parent cycle.";
			return false;
		}
		return true;
	}

	auto DMaterialInstance::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent")
			&& (Event.Phase != EPropertyChangePhase::Committed || Event.Origin != EPropertyChangeOrigin::Edit))
		{
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::ParameterValues);
		}
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return Parent != nullptr ? Parent->GetParameterDefinitions() : std::span<const FMaterialParameterDefinition>{};
	}

	auto DMaterialInstance::GetParameterOverrides() const -> std::span<const FMaterialParameterOverride>
	{
		return ParameterOverrides;
	}

	auto DMaterialInstance::SetImportOwner(const FAssetPath& InOwner) -> void
	{
		if (ImportOwner == InOwner) return;
		ImportOwner = InOwner;
		MarkPackageDirty();
	}

	auto DMaterialInstance::ExchangeImportedState(DMaterialInstance& Other) -> void
	{
		if (&Other == this) return;
		std::swap(Parent, Other.Parent);
		std::swap(ParameterOverrides, Other.ParameterOverrides);
		std::swap(ImportOwner, Other.ImportOwner);
		MarkPackageDirty();
		Other.MarkPackageDirty();
		MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::ParameterValues);
		Other.MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterialInstance::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		if (const FMaterialParameterOverride* Override = FindOverride(ParameterOverrides, Id))
		{
			OutParameter.Definition = Definition;
			OutParameter.Value = Override->Value;
			OutParameter.Source = const_cast<DMaterialInstance*>(this);
			OutParameter.bHasLocalOverride = true;
			return true;
		}
		if (Parent == nullptr || !Parent->ResolveParameterValue(Id, OutParameter)) return false;
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterialInstance::SetParameterOverride(
		const FGuid& Id,
		EMaterialParameterType Type,
		const FMaterialParameterValue& Value
	) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition || Definition->Type != Type) return false;
		const FMaterialParameterValue CanonicalValue = CanonicalizeParameterValue(Type, Value);
		if (FMaterialParameterOverride* Override = FindMutableOverride(ParameterOverrides, Id))
		{
			if (Override->Value == CanonicalValue) return true;
			Override->Value = CanonicalValue;
		}
		else
		{
			ParameterOverrides.push_back({.ParameterId = Id, .Value = CanonicalValue});
		}
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::ClearParameterOverride(const FGuid& Id) -> bool
	{
		const size_t PreviousSize = ParameterOverrides.size();
		std::erase_if(ParameterOverrides, [&Id](const FMaterialParameterOverride& Override) {
			return Override.ParameterId == Id;
		});
		if (ParameterOverrides.size() == PreviousSize) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::HasLocalParameterOverride(const FGuid& Id) const -> bool
	{
		return FindOverride(ParameterOverrides, Id) != nullptr;
	}

	auto DMaterialInstance::IsParameterOverrideOrphan(const FGuid& Id) const -> bool
	{
		return HasLocalParameterOverride(Id) && FindParameterDefinition(Id) == nullptr;
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Vector, FMaterialParameterValue::MakeVector(Value));
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Texture, FMaterialParameterValue::MakeTexture(Value));
	}

	auto DMaterialInstance::ClearScalarParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::ClearVectorParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::ClearTextureParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Texture
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasScalarParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasVectorParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasTextureParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Texture
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.ScalarValue;
		return true;
	}

	auto DMaterialInstance::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.VectorValue;
		return true;
	}

	auto DMaterialInstance::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.TextureValue.Get();
		return true;
	}

	auto DMaterialInstance::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		std::unordered_set<FGuid> OverrideIds;
		for (const FMaterialParameterOverride& Override : ParameterOverrides)
		{
			if (!Override.ParameterId.IsValid())
			{
				OutError = "A material instance asset contains an override with an invalid parameter GUID.";
				return false;
			}
			if (!OverrideIds.insert(Override.ParameterId).second)
			{
				OutError = std::format(
					"A material instance asset contains duplicate overrides for parameter GUID {}.",
					Override.ParameterId.ToString());
				return false;
			}
		}
		if (WouldCreateParentCycle(this, Parent.Get()))
		{
			OutError = "A material instance asset contains a parent cycle.";
			return false;
		}
		return true;
	}
}

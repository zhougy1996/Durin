#include "Materials/MaterialInstance.h"

#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto CanonicalKey(const FMaterialParameterDefinition& Definition) -> std::string
		{
			return Definition.Name.ToString();
		}
	}

	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		for (DMaterialInterface* Candidate = InParent; Candidate != nullptr; Candidate = Candidate->GetParent())
		{
			if (Candidate == this) return false;
		}
		if (Parent == InParent) return true;
		Parent = InParent;
		ReconcileParentDependency();
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
		for (DMaterialInterface* Candidate = CandidateParent; Candidate != nullptr; Candidate = Candidate->GetParent())
		{
			if (Candidate == this)
			{
				OutError = "A material instance cannot create a parent cycle.";
				return false;
			}
		}
		return true;
	}

	auto DMaterialInstance::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent")
			&& (Event.Phase != EPropertyChangePhase::Committed || Event.Origin != EPropertyChangeOrigin::Edit))
		{
			ReconcileParentDependency();
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

	auto DMaterialInstance::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		const std::string Key = CanonicalKey(*Definition);
		OutParameter.Definition = Definition;
		OutParameter.Source = const_cast<DMaterialInstance*>(this);
		OutParameter.bHasLocalOverride = true;
		switch (Definition->Type)
		{
		case EMaterialParameterType::Scalar:
			if (const auto It = ScalarParameterOverrides.find(Key); It != ScalarParameterOverrides.end())
			{
				OutParameter.Value = FMaterialParameterValue::MakeScalar(It->second);
				return true;
			}
			break;
		case EMaterialParameterType::Vector:
			if (const auto It = VectorParameterOverrides.find(Key); It != VectorParameterOverrides.end())
			{
				OutParameter.Value = FMaterialParameterValue::MakeVector(It->second);
				return true;
			}
			break;
		case EMaterialParameterType::Texture:
			if (const auto It = TextureParameterOverrides.find(Key); It != TextureParameterOverrides.end())
			{
				OutParameter.Value = FMaterialParameterValue::MakeTexture(It->second.Get());
				return true;
			}
			break;
		}
		if (Parent == nullptr || !Parent->ResolveParameterValue(Id, OutParameter)) return false;
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		const std::string Key = CanonicalKey(*Definition);
		if (const auto It = ScalarParameterOverrides.find(Key); It != ScalarParameterOverrides.end() && It->second == Value) return true;
		ScalarParameterOverrides[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		const std::string Key = CanonicalKey(*Definition);
		if (const auto It = VectorParameterOverrides.find(Key); It != VectorParameterOverrides.end() && It->second == Value) return true;
		VectorParameterOverrides[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		const std::string Key = CanonicalKey(*Definition);
		if (const auto It = TextureParameterOverrides.find(Key); It != TextureParameterOverrides.end() && It->second.Get() == Value) return true;
		TextureParameterOverrides[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::ClearScalarParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar
			|| ScalarParameterOverrides.erase(CanonicalKey(*Definition)) == 0) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::ClearVectorParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector
			|| VectorParameterOverrides.erase(CanonicalKey(*Definition)) == 0) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::ClearTextureParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture
			|| TextureParameterOverrides.erase(CanonicalKey(*Definition)) == 0) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterialInstance::HasScalarParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& ScalarParameterOverrides.contains(CanonicalKey(*Definition));
	}

	auto DMaterialInstance::HasVectorParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& VectorParameterOverrides.contains(CanonicalKey(*Definition));
	}

	auto DMaterialInstance::HasTextureParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Texture
			&& TextureParameterOverrides.contains(CanonicalKey(*Definition));
	}

	auto DMaterialInstance::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		const auto It = ScalarParameterOverrides.find(CanonicalKey(*Definition));
		if (It != ScalarParameterOverrides.end())
		{
			OutValue = It->second;
			return true;
		}
		return Parent != nullptr && Parent->GetScalarParameterValue(Definition->Name, OutValue);
	}

	auto DMaterialInstance::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		const auto It = VectorParameterOverrides.find(CanonicalKey(*Definition));
		if (It != VectorParameterOverrides.end())
		{
			OutValue = It->second;
			return true;
		}
		return Parent != nullptr && Parent->GetVectorParameterValue(Definition->Name, OutValue);
	}

	auto DMaterialInstance::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		const auto It = TextureParameterOverrides.find(CanonicalKey(*Definition));
		if (It != TextureParameterOverrides.end())
		{
			OutValue = It->second.Get();
			return true;
		}
		return Parent != nullptr && Parent->GetTextureParameterValue(Definition->Name, OutValue);
	}

	auto DMaterialInstance::BeginDestroy() -> void
	{
		if (RegisteredParent != nullptr) RegisteredParent->RemoveDependentInstance(this);
		RegisteredParent = nullptr;
		Parent = nullptr;
		Super::BeginDestroy();
	}

	auto DMaterialInstance::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		for (DMaterialInterface* Candidate = Parent.Get(); Candidate != nullptr; Candidate = Candidate->GetParent())
		{
			if (Candidate == this)
			{
				OutError = "A material instance asset contains a parent cycle.";
				return false;
			}
		}
		ReconcileParentDependency();
		return true;
	}

	auto DMaterialInstance::OnParentRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		MarkRenderDataDirty(DirtyFlags | EMaterialRenderDirtyFlags::ParentChain);
	}

	auto DMaterialInstance::ReconcileParentDependency() -> void
	{
		if (RegisteredParent == Parent) return;
		if (RegisteredParent != nullptr) RegisteredParent->RemoveDependentInstance(this);
		RegisteredParent = Parent;
		if (RegisteredParent != nullptr) RegisteredParent->AddDependentInstance(this);
	}
}

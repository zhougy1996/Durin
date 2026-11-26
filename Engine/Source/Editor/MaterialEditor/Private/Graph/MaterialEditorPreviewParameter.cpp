#include "MaterialEditorPreviewParameter.h"

#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/Material.h"

namespace Durin::MaterialEditor
{
	void DMaterialEditorPreviewParameter::Initialize(FMaterialEditorPreviewParameters& Owner, const FMaterialParameterDefinition& Definition)
	{
		Collection = &Owner;
		ParameterId = Definition.Id;
		Type = Definition.Type;
		switch (Type)
		{
		case EMaterialParameterType::Scalar: Scalar = Definition.Value.GetScalar(); Member = "Scalar"; break;
		case EMaterialParameterType::Vector4: Vector4 = Definition.Value.GetVector4(); Member = "Vector4"; break;
		case EMaterialParameterType::Texture: Texture = Definition.Value.GetTexture(); Member = "Texture"; break;
		}
	}

	auto DMaterialEditorPreviewParameter::GetValue() const -> FMaterialParameterValue
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar: return Scalar;
		case EMaterialParameterType::Vector4: return Vector4;
		case EMaterialParameterType::Texture: return Texture;
		}
		std::unreachable();
	}

	auto DMaterialEditorPreviewParameter::GetEditTarget() -> FPropertyEditTarget
	{
		return FPropertyEditTarget::ForMember(this, GetClass()->FindPropertyByName(Member));
	}

	auto DMaterialEditorPreviewParameter::CanEditChange(const FProperty* Property) const -> bool
	{
		const auto* Owner = Collection ? Collection->Material.Get() : nullptr;
		const auto* Definition = Owner ? Owner->FindParameterDefinition(ParameterId) : nullptr;
		return Super::CanEditChange(Property) && Property->NamePrivate == Member && Definition && Definition->Type == Type;
	}

	auto DMaterialEditorPreviewParameter::PreEditChangeProperty(FPropertyEditProposal& Proposal)
		-> std::expected<void, FObjectValidationError>
	{
		if (auto Base = Super::PreEditChangeProperty(Proposal); !Base) return Base;
		if (!Proposal.DraftRootProperty || !Proposal.DraftRootContainer || Proposal.DraftRootProperty->NamePrivate != Member)
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::Rejected);
		auto* Material = Collection ? Collection->Material.Get() : nullptr;
		const auto* Definition = Material ? Material->FindParameterDefinition(ParameterId) : nullptr;
		if (!Definition || Definition->Type != Type)
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::Rejected);
		auto Candidate = *Definition;
		const auto* Storage = Proposal.DraftRootProperty->GetValuePtr(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		switch (Candidate.Type)
		{
		case EMaterialParameterType::Scalar: Candidate.Value = *static_cast<const float*>(Storage); break;
		case EMaterialParameterType::Vector4: Candidate.Value = *static_cast<const FVector4*>(Storage); break;
		case EMaterialParameterType::Texture: Candidate.Value = *static_cast<const FMaterialTextureValue*>(Storage); break;
		}
		if (auto Valid = ValidateMaterialParameterDefinitions(std::span(&Candidate, 1)); !Valid)
			return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::PropertyRejected,
				.Message = std::string(GetMaterialParameterErrorText(Valid.Error))});
		return {};
	}

	auto DMaterialEditorPreviewParameter::CopyToSourceInstance(bool /*bForceStaticPermutationUpdate*/) -> void
	{
		auto* Owner = Collection ? Collection->Material.Get() : nullptr;
		const auto* Definition = Owner ? Owner->FindParameterDefinition(ParameterId) : nullptr;
		if (!Definition) return;
		Owner->SetParameterValueEditorOnly(Definition->Name, GetValue());
	}

	auto DMaterialEditorPreviewParameter::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if ((Event.ChangeType & EPropertyChangeType::Interactive) || Event.Origin != EPropertyChangeOrigin::Edit) return;
		const TStrongObjectPtr<DMaterial> Owner(Collection ? Collection->Material.Get() : nullptr);
		const auto* Definition = Owner ? Owner->FindParameterDefinition(ParameterId) : nullptr;
		if (!Definition || Definition->Type != Type || Definition->Value == GetValue()) return;
		FScopedMaterialGraphChange Batch(*Owner);
		CopyToSourceInstance();
		Owner->PostEditChangeProperty(Event);
	}

	FMaterialEditorPreviewParameters::FMaterialEditorPreviewParameters(DMaterial* InMaterial) : Material(InMaterial)
	{
		RegenerateArrays();
	}

	FMaterialEditorPreviewParameters::~FMaterialEditorPreviewParameters()
	{
		Clear();
	}

	void FMaterialEditorPreviewParameters::Clear()
	{
		for (const auto& [Id, Parameter] : Parameters)
			if (Parameter) Parameter->Collection = nullptr;
		Parameters.clear();
	}

	void FMaterialEditorPreviewParameters::RegenerateArrays()
	{
		Clear();
		auto* Owner = Material.Get();
		if (!Owner) return;
		if (Owner->GetExpressionCollection().Expressions.empty()) return;
		for (const auto& Definition : Owner->GetParameterDefinitions())
		{
			if (Definition.Type != EMaterialParameterType::Scalar && Definition.Type != EMaterialParameterType::Vector4
				&& Definition.Type != EMaterialParameterType::Texture) continue;
			auto* Parameter = NewObject<DMaterialEditorPreviewParameter>(DMaterialEditorPreviewParameter::StaticClass(),
				nullptr, NAME_None, EObjectFlags::Transient | EObjectFlags::Transactional);
			Parameter->Initialize(*this, Definition);
			Parameters.emplace(Definition.Id, Parameter);
		}
	}

	auto FMaterialEditorPreviewParameters::Find(const FGuid& Id) const -> DMaterialEditorPreviewParameter*
	{
		const auto It = Parameters.find(Id);
		return It != Parameters.end() ? It->second.Get() : nullptr;
	}
}

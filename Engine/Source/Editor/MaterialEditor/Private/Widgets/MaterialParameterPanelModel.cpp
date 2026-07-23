#include "Widgets/MaterialParameterPanelModel.h"

#include "DObject/DurinPropertyTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"

namespace Durin
{
	namespace
	{
		auto FindArrayProperty(DObject* Object, FName Name) -> FArrayProperty*
		{
			FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(Name) : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
				? static_cast<FArrayProperty*>(Property) : nullptr;
		}

		auto CanonicalizeValue(EMaterialParameterType Type, const FMaterialParameterValue& Value)
			-> FMaterialParameterValue
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

		template<typename TEntry, typename TIdMember>
		auto FindScratchEntry(
			const FArrayProperty& Property,
			void* Container,
			uint32 ArrayIndex,
			const FGuid& ParameterId,
			TIdMember IdMember
		) -> TEntry*
		{
			for (uint64 Index = 0; Index < Property.Num(Container, ArrayIndex); ++Index)
			{
				auto* Entry = static_cast<TEntry*>(Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (Entry && Entry->*IdMember == ParameterId) return Entry;
			}
			return nullptr;
		}

		auto SubmitRootArrayEdit(
			FReflectedPropertyView& PropertyView,
			const FReflectedPropertyViewContext& Context,
			DObject* Object,
			FArrayProperty* Property,
			const FGuid& ParameterId,
			EPropertyChangeKind Kind,
			bool bContinuous,
			const std::function<void(const FArrayProperty&, void*, uint32)>& Mutate
		) -> bool
		{
			if (!Object || !Property || !Mutate) return false;
			FReflectedPropertyEditTarget Target = FReflectedPropertyEditTarget::ForMember(Object, Property);
			Target.LogicalIdentity.resize(sizeof(ParameterId));
			std::memcpy(Target.LogicalIdentity.data(), &ParameterId, sizeof(ParameterId));
			Target.Kind = Kind;
			return PropertyView.SubmitPropertyValueEdit(Context, Target,
				[Mutate](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					Mutate(*static_cast<FArrayProperty*>(ScratchProperty), ScratchContainer, ScratchArrayIndex);
				}, bContinuous);
		}

		auto RemoveScratchOverride(
			const FArrayProperty& Property,
			void* Container,
			uint32 ArrayIndex,
			const FGuid& ParameterId
		) -> void
		{
			const uint64 Count = Property.Num(Container, ArrayIndex);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				auto* Entry = static_cast<FMaterialParameterOverride*>(
					Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (!Entry || Entry->ParameterId != ParameterId) continue;
				for (uint64 MoveIndex = Index + 1; MoveIndex < Count; ++MoveIndex)
				{
					auto* Destination = static_cast<FMaterialParameterOverride*>(
						Property.GetMutableElementPtr(Container, MoveIndex - 1, ArrayIndex));
					auto* Source = static_cast<FMaterialParameterOverride*>(
						Property.GetMutableElementPtr(Container, MoveIndex, ArrayIndex));
					*Destination = std::move(*Source);
				}
				Property.Resize(Container, Count - 1, ArrayIndex);
				return;
			}
		}
	}

	FMaterialParameterPanelModel::FMaterialParameterPanelModel(DMaterialInterface* InMaterial)
		: Material(InMaterial)
		, Instance(Cast<DMaterialInstance>(InMaterial))
	{
		if (!Material) return;
		for (const FMaterialParameterDefinition& Definition : Material->GetParameterDefinitions())
		{
			FResolvedMaterialParameter Resolved;
			if (!Material->ResolveParameterValue(Definition.Id, Resolved)) continue;
			Entries.push_back({
				.Definition = Definition,
				.ParameterId = Definition.Id,
				.Value = Resolved.Value,
				.Source = Resolved.Source,
				.Control = SelectControl(Definition),
				.bCanOverride = Instance != nullptr,
				.bHasLocalOverride = Resolved.bHasLocalOverride,
			});
		}
		if (!Instance) return;
		for (const FMaterialParameterOverride& Override : Instance->GetParameterOverrides())
		{
			if (!Instance->IsParameterOverrideOrphan(Override.ParameterId)) continue;
			Entries.push_back({
				.ParameterId = Override.ParameterId,
				.Value = Override.Value,
				.bCanOverride = true,
				.bHasLocalOverride = true,
				.bOrphan = true,
			});
		}
	}

	auto FMaterialParameterPanelModel::SelectControl(const FMaterialParameterDefinition& Definition)
		-> EMaterialParameterControlKind
	{
		switch (Definition.Presentation)
		{
		case EMaterialParameterPresentation::Drag:
			return Definition.Type == EMaterialParameterType::Scalar
				? (Definition.bHasRange ? EMaterialParameterControlKind::RangedScalar
					: EMaterialParameterControlKind::Scalar)
				: EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::Color:
			return Definition.Type == EMaterialParameterType::Vector
				? EMaterialParameterControlKind::Color : EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::AssetPicker:
			return Definition.Type == EMaterialParameterType::Texture
				? EMaterialParameterControlKind::AssetPicker : EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::Default:
			switch (Definition.Type)
			{
			case EMaterialParameterType::Scalar:
				return Definition.bHasRange ? EMaterialParameterControlKind::RangedScalar
					: EMaterialParameterControlKind::Scalar;
			case EMaterialParameterType::Vector: return EMaterialParameterControlKind::Color;
			case EMaterialParameterType::Texture: return EMaterialParameterControlKind::AssetPicker;
			}
		}
		return EMaterialParameterControlKind::Unsupported;
	}

	auto FMaterialParameterPanelModel::SubmitValueEdit(
		FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		const FMaterialParameterValue& Value,
		bool bContinuous
	) const -> bool
	{
		if (!Entry.Definition || Entry.bOrphan || !Material) return false;
		const FMaterialParameterValue CanonicalValue = CanonicalizeValue(Entry.Definition->Type, Value);
		if (Instance)
		{
			if (!Entry.bHasLocalOverride) return false;
			FArrayProperty* Property = FindArrayProperty(Instance, FName("ParameterOverrides"));
			return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
				EPropertyChangeKind::ValueSet, bContinuous,
				[Id = Entry.ParameterId, CanonicalValue](const FArrayProperty& ScratchProperty,
					void* ScratchContainer, uint32 ScratchArrayIndex) {
					if (auto* Override = FindScratchEntry<FMaterialParameterOverride>(
						ScratchProperty, ScratchContainer, ScratchArrayIndex, Id,
						&FMaterialParameterOverride::ParameterId))
					{
						Override->Value = CanonicalValue;
					}
				});
		}
		auto* BaseMaterial = Cast<DMaterial>(Material);
		FArrayProperty* Property = FindArrayProperty(BaseMaterial, FName("ParameterDefinitions"));
		return SubmitRootArrayEdit(PropertyView, Context, BaseMaterial, Property, Entry.ParameterId,
			EPropertyChangeKind::ValueSet, bContinuous,
			[Id = Entry.ParameterId, CanonicalValue](const FArrayProperty& ScratchProperty,
				void* ScratchContainer, uint32 ScratchArrayIndex) {
				if (auto* Definition = FindScratchEntry<FMaterialParameterDefinition>(
					ScratchProperty, ScratchContainer, ScratchArrayIndex, Id,
					&FMaterialParameterDefinition::Id))
				{
					Definition->Value = CanonicalValue;
				}
			});
	}

	auto FMaterialParameterPanelModel::SetOverrideEnabled(
		FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		bool bEnabled
	) const -> bool
	{
		if (!Instance || !Entry.Definition || Entry.bOrphan || Entry.bHasLocalOverride == bEnabled) return false;
		FArrayProperty* Property = FindArrayProperty(Instance, FName("ParameterOverrides"));
		return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
			bEnabled ? EPropertyChangeKind::ArrayAdd : EPropertyChangeKind::ArrayRemove, false,
			[Id = Entry.ParameterId, Value = CanonicalizeValue(Entry.Definition->Type, Entry.Value), bEnabled](
				const FArrayProperty& ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				if (bEnabled)
				{
					const uint64 Count = ScratchProperty.Num(ScratchContainer, ScratchArrayIndex);
					ScratchProperty.Resize(ScratchContainer, Count + 1, ScratchArrayIndex);
					auto* Override = static_cast<FMaterialParameterOverride*>(
						ScratchProperty.GetMutableElementPtr(ScratchContainer, Count, ScratchArrayIndex));
					*Override = {.ParameterId = Id, .Value = Value};
				}
				else
				{
					RemoveScratchOverride(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id);
				}
			});
	}

	auto FMaterialParameterPanelModel::RemoveOrphan(
		FReflectedPropertyView& PropertyView,
		const FReflectedPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry
	) const -> bool
	{
		if (!Instance || !Entry.bOrphan) return false;
		FArrayProperty* Property = FindArrayProperty(Instance, FName("ParameterOverrides"));
		return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
			EPropertyChangeKind::ArrayRemove, false,
			[Id = Entry.ParameterId](const FArrayProperty& ScratchProperty,
				void* ScratchContainer, uint32 ScratchArrayIndex) {
				RemoveScratchOverride(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id);
			});
	}
}

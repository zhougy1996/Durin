#include "Widgets/MaterialParameterPanelModel.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/AssertionMacros.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto FindArrayProperty(DObject* Object, FName Name) -> FArrayProperty*
		{
			FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(Name) : nullptr;
			return Property && Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
				? static_cast<FArrayProperty*>(Property) : nullptr;
		}

		auto CanonicalizeValue(
			const FMaterialParameterDefinition& Definition,
			const FMaterialParameterValue& Value
		)
			-> FMaterialParameterValue
		{
			if (Definition.Type != EMaterialParameterType::Scalar
				|| Definition.Presentation != EMaterialParameterPresentation::Integer) return Value;
			float Scalar = Value.GetScalar();
			if (!std::isfinite(Scalar)) Scalar = Definition.Value.GetScalar();
			if (Definition.bHasRange)
				Scalar = std::clamp(Scalar, Definition.MinimumValue, Definition.MaximumValue);
			return FMaterialParameterValue::MakeScalar(std::floor(Scalar + 0.5f));
		}

		template<typename TEntry>
		auto FindScratchEntry(
			const FArrayProperty& Property,
			void* Container,
			uint32 ArrayIndex,
			const FMaterialParameterInfo& Info
		) -> TEntry*
		{
			for (uint64 Index = 0; Index < Property.Num(Container, ArrayIndex); ++Index)
			{
				auto* Entry = static_cast<TEntry*>(Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (Entry && Entry->ParameterInfo == Info) return Entry;
			}
			return nullptr;
		}

		auto SetOverrideIdentity(FPropertyEditTarget& Target, const FMaterialParameterInfo& Info, const FGuid& OrphanId) -> void
		{
			if (Info.IsGlobal())
			{
				const auto Name = Info.Name.ToString();
				Target.LogicalIdentity.resize(1 + Name.size());
				Target.LogicalIdentity.front() = std::byte{1};
				std::memcpy(Target.LogicalIdentity.data() + 1, Name.data(), Name.size());
			}
			else
			{
				Target.LogicalIdentity.resize(1 + sizeof(OrphanId));
				Target.LogicalIdentity.front() = std::byte{0};
				std::memcpy(Target.LogicalIdentity.data() + 1, &OrphanId, sizeof(OrphanId));
			}
		}

		auto SubmitRootArrayEdit(
			FPropertyView& PropertyView,
			const FPropertyViewContext& Context,
			DObject* Object,
			FArrayProperty* Property,
			const FGuid& ParameterId,
			EPropertyChangeType::Type ChangeType,
			bool bContinuous,
			const std::function<void(const FArrayProperty&, void*, uint32)>& Mutate,
			const FMaterialParameterInfo& Info
		) -> bool
		{
			if (!Object || !Property || !Mutate) return false;
			FPropertyEditTarget Target = FPropertyEditTarget::ForMember(Object, Property);
			SetOverrideIdentity(Target, Info, ParameterId);
			Target.ChangeType = ChangeType;
			const auto Assign = [Mutate](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					Mutate(*static_cast<FArrayProperty*>(ScratchProperty), ScratchContainer, ScratchArrayIndex);
				};
			return PropertyView.SubmitPropertyValueEdit(Context, Target, Assign, bContinuous);
		}

		template<typename TRecord>
		auto RemoveScratchOverride(
			const FArrayProperty& Property,
			void* Container,
			uint32 ArrayIndex,
			const FGuid& ParameterId, const FMaterialParameterInfo& Info
		) -> void
		{
			const uint64 Count = Property.Num(Container, ArrayIndex);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				auto* Entry = static_cast<TRecord*>(
					Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (!Entry || (Info.IsGlobal() ? Entry->ParameterInfo != Info : (Entry->ParameterInfo.IsGlobal() || Entry->ParameterId != ParameterId))) continue;
				for (uint64 MoveIndex = Index + 1; MoveIndex < Count; ++MoveIndex)
				{
					auto* Destination = static_cast<TRecord*>(
						Property.GetMutableElementPtr(Container, MoveIndex - 1, ArrayIndex));
					auto* Source = static_cast<TRecord*>(
						Property.GetMutableElementPtr(Container, MoveIndex, ArrayIndex));
					*Destination = std::move(*Source);
				}
				const auto Resized = Property.Resize(Container, Count - 1, ArrayIndex);
				requiref(Resized.has_value(), "Failed to remove material parameter override: {}", ToString(Resized.error()));
				return;
			}
		}
	}

	FMaterialParameterPanelModel::FMaterialParameterPanelModel(DMaterialInterface* InMaterial)
		: Material(InMaterial)
		, Instance(Cast<DMaterialInstance>(InMaterial))
	{
		ObservedMaterial = InMaterial;
		if (InMaterial)
			ChangeHandle = InMaterial->GetParameterChanges().AddLambda(
				[State = std::weak_ptr<bool>(Invalidated)] {
					if (const auto Dirty = State.lock()) *Dirty = true;
				});
		Refresh();
	}

	FMaterialParameterPanelModel::~FMaterialParameterPanelModel()
	{
		if (auto* Owner = ObservedMaterial.Get()) Owner->GetParameterChanges().Remove(ChangeHandle);
	}

	auto FMaterialParameterPanelModel::Refresh() -> bool
	{
		if (!std::exchange(*Invalidated, false)) return false;
		Material = ObservedMaterial.Get();
		Instance = Cast<DMaterialInstance>(Material);
		Entries.clear();
		if (!Material) return false;
		DMaterial* BaseMaterial = nullptr;
		std::unordered_set<DMaterialInterface*> Visited;
		for (auto* Current = Material; Current && Visited.insert(Current).second;
			Current = Current->GetParent())
		{
			BaseMaterial = Cast<DMaterial>(Current);
			if (BaseMaterial) break;
		}
		std::vector<std::pair<FMaterialParameterInfo, EMaterialParameterType>> Schema;
		for (const auto& Definition : Material->GetParameterDefinitions())
			Schema.emplace_back(Definition.GetParameterInfo(), Definition.Type);
		const auto Reachability = Instance ? Material->GetParameterReachability() : nullptr;
		const bool bRebuildDependencies = !bDependenciesInitialized || !BaseMaterial
			|| BaseMaterial != DependencyMaterial
			|| Schema != DependencySchema || Reachability != DependencyReachability;
		if (bRebuildDependencies)
		{
			DependencyMaterial = BaseMaterial;
			DependencySchema = std::move(Schema);
			DependencyReachability = Reachability;
			bDependenciesInitialized = true;
		}
		for (const auto& Item : Material->GetParameterDefinitions())
		{
			if (Instance && !Reachability->ParameterNames.contains(Item.Name)) continue;
			const auto* Definition = &Item;
			FResolvedMaterialParameter Resolved;
			if (!Material->ResolveParameterValueByInfo(Definition->GetParameterInfo(), Resolved)) continue;
			Entries.push_back({
				.Definition = *Definition,
				.ParameterId = Definition->Id,
				.ParameterInfo = Definition->GetParameterInfo(),
				.Value = Resolved.Value,
				.Source = Resolved.Source,
				.Control = SelectControl(*Definition),
				.bCanOverride = Instance != nullptr,
				.bHasLocalOverride = Resolved.bHasLocalOverride,
			});
		}
		if (!Instance) return bRebuildDependencies;
		Instance->VisitLocalParameterOverrides([&](const FMaterialParameterInfo& Info, const FGuid& Id, const FMaterialParameterValue& Value) {
			const auto* Definition = Material->FindParameterDefinitionByInfo(Info);
			if (Definition && Definition->Value.HasSameType(Value) && Reachability->ParameterNames.contains(Definition->Name)) return;
			Entries.push_back({.ParameterId = Id, .ParameterInfo = Info, .Value = Value, .bCanOverride = true,
				.bHasLocalOverride = true, .bOrphan = true});
		});
		return bRebuildDependencies;
	}

	auto FMaterialParameterPanelModel::SelectControl(const FMaterialParameterDefinition& Definition)
		-> EMaterialParameterControlKind
	{
		switch (Definition.Presentation)
		{
		case EMaterialParameterPresentation::Drag:
			if (Definition.Type == EMaterialParameterType::Scalar)
				return Definition.bHasRange ? EMaterialParameterControlKind::RangedScalar : EMaterialParameterControlKind::Scalar;
			return (Definition.Type == EMaterialParameterType::Vector4)
				? EMaterialParameterControlKind::Vector : EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::Integer:
			return Definition.Type == EMaterialParameterType::Scalar
				? EMaterialParameterControlKind::IntegerScalar : EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::Color:
			return Definition.Type == EMaterialParameterType::Vector4
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
			case EMaterialParameterType::Vector4: return EMaterialParameterControlKind::Vector;
			case EMaterialParameterType::Texture: return EMaterialParameterControlKind::AssetPicker;
			case EMaterialParameterType::Vector:
			case EMaterialParameterType::Vector2: return EMaterialParameterControlKind::Unsupported;
			}
		}
		return EMaterialParameterControlKind::Unsupported;
	}

	auto FMaterialParameterPanelModel::SubmitValueEdit(
		FPropertyView& PropertyView,
		const FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		const FMaterialParameterValue& Value,
		bool bContinuous
	) const -> bool
	{
		FPropertyEditTarget Target;
		std::function<void(FProperty*, void*, uint32)> Assign;
		return PrepareValueEdit(Entry, &Value, Target, Assign)
			&& PropertyView.SubmitPropertyValueEdit(Context, Target, Assign, bContinuous);
	}

	auto FMaterialParameterPanelModel::UpdateValueEdit(FPropertyView& PropertyView, const FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry, const MonaImGui::PropertyEdit::FPropertyInteractionState& Interaction,
		const FMaterialParameterValue* ProposedValue) const -> bool
	{
		FPropertyEditTarget Target;
		std::function<void(FProperty*, void*, uint32)> Assign;
		return PrepareValueEdit(Entry, ProposedValue, Target, Assign)
			&& PropertyView.UpdatePropertyValueEdit(Context, Target, Interaction, Assign);
	}

	auto FMaterialParameterPanelModel::PrepareValueEdit(const FMaterialParameterPanelEntry& Entry,
		const FMaterialParameterValue* Value, FPropertyEditTarget& Target,
		std::function<void(FProperty*, void*, uint32)>& Assign) const -> bool
	{
		if (!Entry.Definition || Entry.bOrphan || !Material || !Instance || !Entry.bHasLocalOverride) return false;
		if (Value && (Entry.Definition->Type != Value->GetType()
			|| (Value->GetType() == EMaterialParameterType::Texture && !Value->GetTexture().IsValid()))) return false;
		return VisitMaterialParameterValueType(Entry.Definition->Type, [&]<typename TRecord>() {
			FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
			if (!Property) return false;
			Target = FPropertyEditTarget::ForMember(Instance, Property);
			SetOverrideIdentity(Target, Entry.ParameterInfo, {});
			if (Value)
				Assign = [Info = Entry.ParameterInfo, CanonicalValue = CanonicalizeValue(*Entry.Definition, *Value)](
					FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					if (auto* Override = FindScratchEntry<TRecord>(*static_cast<FArrayProperty*>(ScratchProperty),
						ScratchContainer, ScratchArrayIndex, Info)) Override->SetValue(CanonicalValue);
				};
			return true;
		});
	}

	auto FMaterialParameterPanelModel::SetOverrideEnabled(
		FPropertyView& PropertyView,
		const FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		bool bEnabled
	) const -> bool
	{
		if (!Instance || !Entry.Definition || Entry.bOrphan || Entry.bHasLocalOverride == bEnabled) return false;
		return VisitMaterialParameterValueType(Entry.Definition->Type, [&]<typename TRecord>() {
			FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
			return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
				bEnabled ? EPropertyChangeType::ArrayAdd : EPropertyChangeType::ArrayRemove, false,
				[Id = Entry.ParameterId, Info = Entry.Definition->GetParameterInfo(), Value = CanonicalizeValue(*Entry.Definition, Entry.Value), bEnabled](
					const FArrayProperty& ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					if (bEnabled)
					{
						const uint64 Count = ScratchProperty.Num(ScratchContainer, ScratchArrayIndex);
						const auto Resized = ScratchProperty.Resize(ScratchContainer, Count + 1, ScratchArrayIndex);
						requiref(Resized.has_value(), "Failed to add material parameter override: {}", ToString(Resized.error()));
						auto* Override = static_cast<TRecord*>(
							ScratchProperty.GetMutableElementPtr(ScratchContainer, Count, ScratchArrayIndex));
						Override->ParameterId = Id;
						Override->ParameterInfo = Info;
						Override->SetValue(Value);
					}
					else RemoveScratchOverride<TRecord>(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id, Info);
				}, Entry.ParameterInfo);
		});
	}

	auto FMaterialParameterPanelModel::RemoveOrphan(
		FPropertyView& PropertyView,
		const FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry
	) const -> bool
	{
		if (!Instance || !Entry.bOrphan) return false;
		if (Entry.ParameterInfo.IsGlobal() && !Instance->IsParameterValueOrphanByInfo(Entry.ParameterInfo)) return false;
		return VisitMaterialParameterValueType(Entry.Value.GetType(), [&]<typename TRecord>() {
			FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
			return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
				EPropertyChangeType::ArrayRemove, false,
				[Id = Entry.ParameterId, Info = Entry.ParameterInfo](const FArrayProperty& ScratchProperty,
					void* ScratchContainer, uint32 ScratchArrayIndex) {
					RemoveScratchOverride<TRecord>(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id, Info);
				}, Entry.ParameterInfo);
		});
	}
}

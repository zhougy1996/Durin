#include "Widgets/MaterialParameterPanelModel.h"
#include "Graph/MaterialExpressionInputs.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"

namespace Durin::Editor::Material
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
			::Durin::Editor::FPropertyView& PropertyView,
			const ::Durin::Editor::FPropertyViewContext& Context,
			DObject* Object,
			FArrayProperty* Property,
			const FGuid& ParameterId,
			EPropertyChangeKind Kind,
			bool bContinuous,
			const std::function<void(const FArrayProperty&, void*, uint32)>& Mutate
		) -> bool
		{
			if (!Object || !Property || !Mutate) return false;
			::Durin::Editor::FPropertyEditTarget Target = ::Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
			Target.LogicalIdentity.resize(sizeof(ParameterId));
			std::memcpy(Target.LogicalIdentity.data(), &ParameterId, sizeof(ParameterId));
			Target.Kind = Kind;
			return PropertyView.SubmitPropertyValueEdit(Context, Target,
				[Mutate](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					Mutate(*static_cast<FArrayProperty*>(ScratchProperty), ScratchContainer, ScratchArrayIndex);
				}, bContinuous);
		}

		template<typename TRecord>
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
				auto* Entry = static_cast<TRecord*>(
					Property.GetMutableElementPtr(Container, Index, ArrayIndex));
				if (!Entry || Entry->ParameterId != ParameterId) continue;
				for (uint64 MoveIndex = Index + 1; MoveIndex < Count; ++MoveIndex)
				{
					auto* Destination = static_cast<TRecord*>(
						Property.GetMutableElementPtr(Container, MoveIndex - 1, ArrayIndex));
					auto* Source = static_cast<TRecord*>(
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
		Refresh();
	}

	auto FMaterialParameterPanelModel::Refresh() -> bool
	{
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
		std::vector<std::pair<FGuid, EMaterialParameterType>> Schema;
		for (const auto& Definition : Material->GetParameterDefinitions())
			Schema.emplace_back(Definition.Id, Definition.Type);
		const uint64 ProgramRevision = BaseMaterial ? BaseMaterial->GetMaterialProgramRevision() : 0;
		const bool bRebuildDependencies = !bDependenciesInitialized || !BaseMaterial
			|| BaseMaterial != DependencyMaterial || ProgramRevision != DependencyProgramRevision
			|| Schema != DependencySchema;
		if (bRebuildDependencies)
		{
			DependencyMaterial = BaseMaterial;
			DependencyProgramRevision = ProgramRevision;
			DependencySchema = std::move(Schema);
			ParameterIds.clear();
			ReachableParameterIds.clear();
			if (Instance && BaseMaterial)
			{
				std::unordered_map<FGuid, DMaterialExpression*> Expressions;
				for (const auto& Expression : BaseMaterial->GetExpressionCollection().Expressions) Expressions.emplace(Expression->Id, Expression.Get());
				std::unordered_set<FGuid> Visited;
				const auto AddParameter = [&](const DMaterialExpressionParameter* Parameter) {
					if (Parameter && Parameter->Metadata.Id.IsValid() && Material->FindParameterDefinition(Parameter->Metadata.Id)
						&& ReachableParameterIds.insert(Parameter->Metadata.Id).second) ParameterIds.push_back(Parameter->Metadata.Id);
				};
				std::function<void(const FMaterialExpressionInput&)> Visit = [&](const FMaterialExpressionInput& Input) {
					const auto It = Expressions.find(Input.ExpressionId);
					if (It == Expressions.end()) return;
					auto* Expression = It->second;
					const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression);
					if (Cast<DMaterialExpressionTextureSampleParameter2D>(Expression) && Input.OutputIndex == 7)
					{
						AddParameter(Parameter); // A resource-only use does not evaluate this sample's UV branch.
						return;
					}
					if (!Visited.insert(Input.ExpressionId).second) return;
					VisitMaterialExpressionInputs(*Expression, [&](uint32, FMaterialExpressionInput& Source) { Visit(Source); });
					AddParameter(Parameter);
				};
				const auto& Outputs = BaseMaterial->GetExpressionOutputs();
				for (const auto* Output : {&Outputs.Surface, &Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic,
					&Outputs.Roughness, &Outputs.AmbientOcclusion, &Outputs.Emissive, &Outputs.Opacity, &Outputs.OpacityMask}) Visit(*Output);
			}
			else if (!Instance)
				for (const auto& Definition : Material->GetParameterDefinitions())
					ParameterIds.push_back(Definition.Id);
			bDependenciesInitialized = true;
		}
		for (const FGuid& ParameterId : ParameterIds)
		{
			const FMaterialParameterDefinition* Definition =
				Material->FindParameterDefinition(ParameterId);
			if (!Definition) continue;
			FResolvedMaterialParameter Resolved;
			if (!Material->ResolveParameterValue(Definition->Id, Resolved)) continue;
			Entries.push_back({
				.Definition = *Definition,
				.ParameterId = Definition->Id,
				.Value = Resolved.Value,
				.Source = Resolved.Source,
				.Control = SelectControl(*Definition),
				.bCanOverride = Instance != nullptr,
				.bHasLocalOverride = Resolved.bHasLocalOverride,
			});
		}
		if (!Instance) return bRebuildDependencies;
		Instance->VisitLocalParameterValues([&](const FGuid& Id, const FMaterialParameterValue& Value) {
			const auto* Definition = Material->FindParameterDefinition(Id);
			if (Definition && Definition->Type == Value.GetType() && ReachableParameterIds.contains(Id)) return;
			Entries.push_back({.ParameterId = Id, .Value = Value, .bCanOverride = true,
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
			return (Definition.Type == EMaterialParameterType::Vector
				|| Definition.Type == EMaterialParameterType::Vector2
				|| Definition.Type == EMaterialParameterType::Vector4)
				? EMaterialParameterControlKind::Vector : EMaterialParameterControlKind::Unsupported;
		case EMaterialParameterPresentation::Integer:
			return Definition.Type == EMaterialParameterType::Scalar
				? EMaterialParameterControlKind::IntegerScalar : EMaterialParameterControlKind::Unsupported;
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
			case EMaterialParameterType::Vector2: return EMaterialParameterControlKind::Vector;
			case EMaterialParameterType::Vector4: return EMaterialParameterControlKind::Vector;
			case EMaterialParameterType::Vector: return EMaterialParameterControlKind::Color;
			case EMaterialParameterType::Texture: return EMaterialParameterControlKind::AssetPicker;
			}
		}
		return EMaterialParameterControlKind::Unsupported;
	}

	auto FMaterialParameterPanelModel::SubmitValueEdit(
		::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		const FMaterialParameterValue& Value,
		bool bContinuous
	) const -> bool
	{
		if (!Entry.Definition || Entry.bOrphan || !Material || Entry.Definition->Type != Value.GetType()) return false;
		if (Entry.Definition->Type == EMaterialParameterType::Texture
			&& !IsValidMaterialSampling(Value.GetTexture().SamplerState, Value.GetTexture().TextureFallback)) return false;
		const FMaterialParameterValue CanonicalValue = CanonicalizeValue(*Entry.Definition, Value);
		if (Instance)
		{
			if (!Entry.bHasLocalOverride) return false;
			return VisitMaterialParameterValueType(Entry.Definition->Type, [&]<typename TRecord>() {
				FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
				return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
					EPropertyChangeKind::ValueSet, bContinuous,
					[Id = Entry.ParameterId, CanonicalValue](const FArrayProperty& ScratchProperty,
						void* ScratchContainer, uint32 ScratchArrayIndex) {
						if (auto* Override = FindScratchEntry<TRecord>(ScratchProperty, ScratchContainer,
							ScratchArrayIndex, Id, &TRecord::ParameterId)) Override->SetValue(CanonicalValue);
					});
			});
		}
		return false;
	}

	auto FMaterialParameterPanelModel::SetOverrideEnabled(
		::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry,
		bool bEnabled
	) const -> bool
	{
		if (!Instance || !Entry.Definition || Entry.bOrphan || Entry.bHasLocalOverride == bEnabled) return false;
		return VisitMaterialParameterValueType(Entry.Definition->Type, [&]<typename TRecord>() {
			FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
			return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
				bEnabled ? EPropertyChangeKind::ArrayAdd : EPropertyChangeKind::ArrayRemove, false,
				[Id = Entry.ParameterId, Value = CanonicalizeValue(*Entry.Definition, Entry.Value), bEnabled](
					const FArrayProperty& ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					if (bEnabled)
					{
						const uint64 Count = ScratchProperty.Num(ScratchContainer, ScratchArrayIndex);
						ScratchProperty.Resize(ScratchContainer, Count + 1, ScratchArrayIndex);
						auto* Override = static_cast<TRecord*>(
							ScratchProperty.GetMutableElementPtr(ScratchContainer, Count, ScratchArrayIndex));
						Override->ParameterId = Id;
						Override->SetValue(Value);
					}
					else RemoveScratchOverride<TRecord>(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id);
				});
		});
	}

	auto FMaterialParameterPanelModel::RemoveOrphan(
		::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& Context,
		const FMaterialParameterPanelEntry& Entry
	) const -> bool
	{
		if (!Instance || !Entry.bOrphan) return false;
		FMaterialParameterValue Override;
		if (!Instance->GetLocalParameterValue(Entry.ParameterId, Override)) return false;
		return VisitMaterialParameterValueType(Override.GetType(), [&]<typename TRecord>() {
			FArrayProperty* Property = FindArrayProperty(Instance, TRecord::PropertyName());
			return SubmitRootArrayEdit(PropertyView, Context, Instance, Property, Entry.ParameterId,
				EPropertyChangeKind::ArrayRemove, false,
				[Id = Entry.ParameterId](const FArrayProperty& ScratchProperty,
					void* ScratchContainer, uint32 ScratchArrayIndex) {
					RemoveScratchOverride<TRecord>(ScratchProperty, ScratchContainer, ScratchArrayIndex, Id);
				});
		});
	}
}

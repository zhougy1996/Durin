#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	MIR::FModule::FModule()
	{
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			AttributeRoots.push_back({.AttributeID = Definition.AttributeID,
				.Type = Definition.Semantics.Type, .LegalStages = Definition.Semantics.Stages,
				.SpatialKind = Definition.Semantics.Kind, .CoordinateSpace = Definition.Semantics.Space,
				.Literal = Definition.DefaultValue});
	}
	auto MIR::FModule::GetAttribute(EMaterialProperty Property) -> FAttributeRoot&
	{
		return const_cast<FAttributeRoot&>(std::as_const(*this).GetAttribute(Property));
	}
	auto MIR::FModule::GetAttribute(EMaterialProperty Property) const -> const FAttributeRoot&
	{
		const auto ID = FMaterialAttributeDefinitionMap::GetID(Property);
		for (const auto& Root : AttributeRoots) if (Root.AttributeID == ID) return Root;
		check(false);
		return AttributeRoots.front();
	}
	auto FMaterialAttributeDefinitionMap::GetDefinitions() -> std::span<const FMaterialAttributeDefinition>
	{
		using P = EMaterialProperty;
		using T = EMaterialProgramValueType;
		using S = EMaterialEvaluationStage;
		static const std::array Definitions{
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,1}, P::MP_BaseColor, "Base Color", {T::Float3,S::Pixel}, {.5f,.5f,.5f}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,2}, P::MP_Normal, "Normal", {T::Float3,S::Pixel,EMaterialSpatialKind::Normal,EMaterialCoordinateSpace::Tangent}, {0,0,1}, EMaterialAttributeBlend::NormalizedNormal},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,3}, P::MP_Metallic, "Metallic", {T::Float,S::Pixel}, {0}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,4}, P::MP_Roughness, "Roughness", {T::Float,S::Pixel}, {.5f}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,5}, P::MP_AmbientOcclusion, "Ambient Occlusion", {T::Float,S::Pixel}, {1}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,6}, P::MP_EmissiveColor, "Emissive Color", {T::Float3,S::Pixel}, {0,0,0}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,7}, P::MP_Opacity, "Opacity", {T::Float,S::Pixel}, {1}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,8}, P::MP_OpacityMask, "Opacity Mask", {T::Float,S::Pixel}, {1}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,9}, P::MP_WorldPositionOffset, "World Position Offset", {T::Float3,S::Vertex,EMaterialSpatialKind::Direction,EMaterialCoordinateSpace::World}, {0,0,0}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,10}, P::MP_Specular, "Specular", {T::Float,S::Pixel}, {.5f}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,11}, P::MP_ClearCoat, "Clear Coat", {T::Float,S::Pixel}, {1}},
			FMaterialAttributeDefinition{{0x4d415454,0x52505250,1,12}, P::MP_ClearCoatRoughness, "Clear Coat Roughness", {T::Float,S::Pixel}, {.1f}},
		};
		return Definitions;
	}
	auto FMaterialAttributeDefinitionMap::Find(FGuid ID) -> const FMaterialAttributeDefinition*
	{
		for (const auto& Definition : GetDefinitions()) if (Definition.AttributeID == ID) return &Definition;
		return nullptr;
	}
	auto FMaterialAttributeDefinitionMap::Find(EMaterialProperty Property) -> const FMaterialAttributeDefinition*
	{
		for (const auto& Definition : GetDefinitions()) if (Definition.Property == Property) return &Definition;
		return nullptr;
	}
	auto FMaterialAttributeDefinitionMap::GetID(EMaterialProperty Property) -> FGuid
	{
		const auto* Definition = Find(Property);
		return Definition ? Definition->AttributeID : FGuid{};
	}
	auto FMaterialAttributeDefinitionMap::GetActiveProperties(EMaterialDomain Domain,
		EMaterialShadingModel Model, EMaterialBlendMode Blend) -> std::vector<EMaterialProperty>
	{
		using P = EMaterialProperty;
		if (Domain != EMaterialDomain::Surface && Domain != EMaterialDomain::PostProcess) return {};
		if (Model != EMaterialShadingModel::DefaultLit && Model != EMaterialShadingModel::Unlit && Model != EMaterialShadingModel::ClearCoat) return {};
		if (Blend != EMaterialBlendMode::Opaque && Blend != EMaterialBlendMode::Masked && Blend != EMaterialBlendMode::Translucent) return {};
		if (Domain == EMaterialDomain::PostProcess)
			return Model == EMaterialShadingModel::Unlit && Blend == EMaterialBlendMode::Opaque
				? std::vector<P>{P::MP_EmissiveColor} : std::vector<P>{};
		if (Model == EMaterialShadingModel::ClearCoat && Blend == EMaterialBlendMode::Translucent) return {};
		std::vector<P> Result{P::MP_EmissiveColor, P::MP_WorldPositionOffset};
		if (Model != EMaterialShadingModel::Unlit)
			Result.insert(Result.end(), {P::MP_BaseColor,P::MP_Normal,P::MP_Metallic,P::MP_Roughness,P::MP_AmbientOcclusion,P::MP_Specular});
		if (Model == EMaterialShadingModel::ClearCoat) Result.insert(Result.end(), {P::MP_ClearCoat, P::MP_ClearCoatRoughness});
		if (Blend == EMaterialBlendMode::Translucent) Result.push_back(P::MP_Opacity);
		if (Blend == EMaterialBlendMode::Masked) Result.push_back(P::MP_OpacityMask);
		return Result;
	}
}

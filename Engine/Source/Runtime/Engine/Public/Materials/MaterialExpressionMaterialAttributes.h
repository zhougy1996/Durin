#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionMaterialAttributes.gen.h"

namespace Durin
{
	// Unconnected fields inherit the material attribute definition.
	DCLASS()
	class DMaterialExpressionMakeMaterialAttributes : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeMaterialAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput BaseColor;
		DPROPERTY()
		FMaterialExpressionInput Normal;
		DPROPERTY()
		FMaterialExpressionInput Metallic;
		DPROPERTY()
		FMaterialExpressionInput Roughness;
		DPROPERTY()
		FMaterialExpressionInput AmbientOcclusion;
		DPROPERTY()
		FMaterialExpressionInput EmissiveColor;
		DPROPERTY()
		FMaterialExpressionInput Opacity;
		DPROPERTY()
		FMaterialExpressionInput OpacityMask;
		DPROPERTY()
		FMaterialExpressionInput WorldPositionOffset;
		DPROPERTY()
		FMaterialExpressionInput Specular;
		DPROPERTY()
		FMaterialExpressionInput ClearCoat;
		DPROPERTY()
		FMaterialExpressionInput ClearCoatRoughness;
		DPROPERTY()
		FMaterialExpressionInput Refraction;


		ENGINE_API auto GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*;
		auto GetExpressionInputForProperty(EMaterialProperty Property) -> FMaterialExpressionInput*
			{ return const_cast<FMaterialExpressionInput*>(std::as_const(*this).GetExpressionInputForProperty(Property)); }

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::MakeMaterialAttributes, .Name = "Make Material Attributes",
				.Description = "Constructs typed material attributes using the shared property definitions.",
				.Category = "Surface", .FixedOutputType = EMaterialProgramValueType::MaterialAttributes};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			const auto Definitions = FMaterialAttributeDefinitionMap::GetDefinitions();
			if (Index < 0 || static_cast<size_t>(Index) >= Definitions.size()) return {};
			const auto& Definition = Definitions[Index];
			return {Definition.Name, GetMaterialSurfaceOutputType(Definition.Property)};
		}
#if DURIN_WITH_EDITORONLY_DATA
		auto IsInputConnectionRequired(int32 Index) const -> bool override { return false; }
		auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override
		{
			return DMaterialExpression::GetOutput(Index);
		}
#endif
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		auto GetAuthoredInputCount() const -> uint32 override { return uint32(FMaterialAttributeDefinitionMap::GetDefinitions().size()); }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Selects stable output attributes from one aggregate surface.
	DCLASS()
	class DMaterialExpressionGetMaterialAttributes : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialExpressionGetMaterialAttributes(const FObjectInitializer& Initializer);

		DPROPERTY()
		FMaterialExpressionInput Surface;

		DPROPERTY()
		std::vector<FGuid> AttributeIDs;
		auto IsSelected(EMaterialProperty Property) const -> bool
			{ return std::ranges::contains(AttributeIDs, FMaterialAttributeDefinitionMap::GetID(Property)); }

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::GetMaterialAttributes, .Name = "Get Material Attributes",
				.Description = "Reads selected typed material attributes.",
				.Category = "Surface", .bHasInstanceShape = true};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0)
				return {.Name = "Material Attributes", .Type = EMaterialProgramValueType::MaterialAttributes,
					.CatalogAlias = "Attributes"};
			return {};
		}
#if DURIN_WITH_EDITORONLY_DATA
		auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override
		{
			if (Index < 0) return {};
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			{
				if (!std::ranges::contains(AttributeIDs, Definition.AttributeID)) continue;
				if (Index-- == 0)
					return FMaterialExpressionOutput{Definition.Name, Definition.Semantics.Type,
						static_cast<uint8>(Definition.Property), Definition.AttributeID};
			}
			return {};
		}
#endif
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	DSTRUCT()
	struct FMaterialExpressionAttributeBinding
	{
		GENERATED_BODY()
		FMaterialExpressionAttributeBinding() = default;
		FMaterialExpressionAttributeBinding(EMaterialProperty InAttribute, FMaterialExpressionInput InSource)
			: AttributeID(FMaterialAttributeDefinitionMap::GetID(InAttribute))
		{
			Input = InSource;
		}

		DPROPERTY()
		FGuid AttributeID = FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_BaseColor);
		auto GetProperty() const -> EMaterialProperty
		{
			const auto* Definition = FMaterialAttributeDefinitionMap::Find(AttributeID);
			return Definition ? Definition->Property : static_cast<EMaterialProperty>(0xff);
		}

		DPROPERTY()
		FMaterialExpressionInput Input;


		auto operator==(const FMaterialExpressionAttributeBinding&) const -> bool = default;
	};

	// Overrides only selected attributes on one aggregate surface.
	DCLASS()
	class DMaterialExpressionSetMaterialAttributes : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSetMaterialAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Surface;

		DPROPERTY()
		std::vector<FMaterialExpressionAttributeBinding> Attributes;

#if DURIN_WITH_EDITORONLY_DATA
		auto IsInputConnectionRequired(int32 Index) const -> bool override { return false; }
#endif
		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::SetMaterialAttributes, .Name = "Set Material Attributes",
				.Description = "Overrides connected attributes on the incoming surface. An open surface input uses the material attribute defaults.",
				.Category = "Surface",
				.FixedOutputType = EMaterialProgramValueType::MaterialAttributes, .bHasInstanceShape = true};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0)
				return {.Name = "Material Attributes", .Type = EMaterialProgramValueType::MaterialAttributes,
					.CatalogAlias = "Attributes"};
			// Override inputs use their stored material property selector plus one.
			if (Index < 0 || !std::in_range<uint8>(Index - 1)) return {};
			const auto Property = static_cast<EMaterialProperty>(Index - 1);
			const auto* Definition = FMaterialAttributeDefinitionMap::Find(Property);
			if (!Definition || !std::ranges::contains(Attributes, Definition->AttributeID, &FMaterialExpressionAttributeBinding::AttributeID))
				return {};
			return {.Name = Definition->Name, .Type = GetMaterialSurfaceOutputType(Property),
				.bAllowImplicitCast = MaterialInputAllowsValidCast(EMaterialProgramOpcode::MakeMaterialAttributes, static_cast<uint32>(Property))};
		}

		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		auto GetAuthoredInputCount() const -> uint32 override { return 1 + static_cast<uint32>(Attributes.size()); }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	DCLASS()
	class DMaterialExpressionBlendMaterialAttributes : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionBlendMaterialAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		FMaterialExpressionInput A;
		DPROPERTY()
		FMaterialExpressionInput B;
		DPROPERTY()
		FMaterialNumericInput Alpha;
		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::BlendMaterialAttributes, .Name = "Blend Material Attributes",
				.Description = "Blends numeric attributes and safely normalizes blended normals.",
				.Category = "Surface", .FixedOutputType = EMaterialProgramValueType::MaterialAttributes};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			using Type = EMaterialProgramValueType;
			switch (Index)
			{
			case 0: return {"A", Type::MaterialAttributes};
			case 1: return {"B", Type::MaterialAttributes};
			case 2: return {"Alpha", Type::Float};
			default: return {};
			}
		}
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}

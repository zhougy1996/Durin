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
		FGuid AttributeID{0x4d415454, 0x52505250, 1, 1};
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
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}

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
	// Builds eight surface attributes from their applicable numeric inputs.
	DCLASS()
	class DMaterialExpressionMakeMaterialAttributes : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeMaterialAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialAttributeRoot Values;
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
			Source.Connection = InSource;
			Source.Constant.resize(static_cast<uint32>(GetMaterialSurfaceOutputType(InAttribute)) + 1, 0.f);
		}

		DPROPERTY()
		FGuid AttributeID{0x4d415454, 0x52505250, 1, 1};
		auto GetProperty() const -> EMaterialProperty
		{
			const auto* Definition = FMaterialAttributeDefinitionMap::Find(AttributeID);
			return Definition ? Definition->Property : static_cast<EMaterialProperty>(0xff);
		}

		DPROPERTY()
		FMaterialNumericInput Source;

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

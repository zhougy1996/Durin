#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	enum class EMaterialAttributeBlend : uint8 { Linear, NormalizedNormal };
	struct FMaterialAttributeDefinition
	{
		FGuid AttributeID;
		EMaterialProperty Property;
		std::string_view Name;
		FMaterialValueSemantics Semantics;
		FMaterialProgramLiteral DefaultValue;
		EMaterialAttributeBlend Blend = EMaterialAttributeBlend::Linear;
	};
	class FMaterialAttributeDefinitionMap
	{
	public:
		ENGINE_API static auto GetDefinitions() -> std::span<const FMaterialAttributeDefinition>;
		ENGINE_API static auto Find(FGuid AttributeID) -> const FMaterialAttributeDefinition*;
		ENGINE_API static auto Find(EMaterialProperty Property) -> const FMaterialAttributeDefinition*;
		ENGINE_API static auto GetID(EMaterialProperty Property) -> FGuid;
		ENGINE_API static auto GetActiveProperties(EMaterialDomain Domain,
			EMaterialShadingModel ShadingModel, EMaterialBlendMode BlendMode) -> std::vector<EMaterialProperty>;
	};
}

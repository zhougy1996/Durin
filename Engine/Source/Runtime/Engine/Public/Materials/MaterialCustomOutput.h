#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	enum class EMaterialShaderEntry : uint8;
	struct FMaterialCustomOutputSlot
	{
		FGuid Id;
		std::string_view Name;
		FMaterialValueSemantics Semantics;
		FMaterialProgramLiteral DefaultValue;
	};
	struct FMaterialCustomOutputDefinition
	{
		std::string_view FunctionName;
		std::string_view DisplayName;
		EMaterialShadingModel ShadingModel;
		EMaterialShaderEntry Entry;
		std::span<const FMaterialCustomOutputSlot> Outputs;
	};
	namespace SingleLayerWater
	{
		inline constexpr FGuid Scattering{0x4d415454,0x52505250,1,14};
		inline constexpr FGuid Absorption{0x4d415454,0x52505250,1,15};
		inline constexpr FGuid PhaseG{0x4d415454,0x52505250,1,16};
		inline constexpr FGuid ColorScale{0x4d415454,0x52505250,1,17};
		ENGINE_API auto GetOutputDefinition() -> const FMaterialCustomOutputDefinition&;
	}
	ENGINE_API auto GetMaterialCustomOutputs() -> std::span<const FMaterialCustomOutputDefinition>;
	ENGINE_API auto FindMaterialCustomOutputSlot(FGuid Id) -> const FMaterialCustomOutputSlot*;
}

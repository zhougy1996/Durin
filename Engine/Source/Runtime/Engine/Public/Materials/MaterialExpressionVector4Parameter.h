#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressionVector4Parameter.gen.h"

namespace Durin
{
	// Owns a concrete vector4 parameter value.
	DCLASS()
	class DMaterialExpressionVector4Parameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector4Parameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Parameter, .Name = "Vector Parameter",
				.Description = "Create a new numeric parameter exposed to material instances.",
				.Category = "Parameters", .FixedOutputType = EMaterialProgramValueType::Float4};
		}

		DPROPERTY()
		FVector4 DefaultValue{0.0};

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto SetParameterValue(const FName& Name, const FMaterialParameterMetadata& Meta,
			EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::None) -> bool override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}

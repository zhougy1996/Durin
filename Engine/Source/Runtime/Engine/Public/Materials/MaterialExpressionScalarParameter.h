#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressionScalarParameter.gen.h"

namespace Durin
{
	// Owns a concrete scalar parameter value.
	DCLASS()
	class DMaterialExpressionScalarParameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionScalarParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Parameter, .Name = "Scalar Parameter",
				.Description = "Create a new numeric parameter exposed to material instances.",
				.Category = "Parameters", .FixedOutputType = EMaterialProgramValueType::Float};
		}

		DPROPERTY()
		float DefaultValue = 0.0f;

		DPROPERTY()
		bool bHasRange = false;

		DPROPERTY()
		float MinimumValue = 0.0f;

		DPROPERTY()
		float MaximumValue = 0.0f;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto SetParameterValue(const FName& Name, const FMaterialParameterMetadata& Meta,
			EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::None) -> bool override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}

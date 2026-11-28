#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionVertexInterpolator.gen.h"

namespace Durin
{
	DCLASS()
	class DMaterialExpressionVertexInterpolator : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVertexInterpolator(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::VertexInterpolator, .Name = "Vertex Interpolator",
				.Description = "Evaluates the input per vertex and interpolates its value to pixel calculations.",
				.Category = "Vertex"};
		}

		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput Input;
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}

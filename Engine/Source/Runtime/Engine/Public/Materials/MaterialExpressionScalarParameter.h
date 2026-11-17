#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionParameter.h"
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

		DPROPERTY()
		float DefaultValue = 0.0f;

		DPROPERTY()
		bool bHasRange = false;

		DPROPERTY()
		float MinimumValue = 0.0f;

		DPROPERTY()
		float MaximumValue = 0.0f;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}

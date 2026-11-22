#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"

#include "MaterialExpressionSingleLayerWaterOutput.gen.h"

namespace Durin
{
	// Material output terminal. Its inputs become independent pixel roots.
	DCLASS()
	class DMaterialExpressionSingleLayerWaterOutput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSingleLayerWaterOutput(const FObjectInitializer& Initializer)
			: Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput ScatteringCoefficients;
		DPROPERTY()
		FMaterialExpressionInput AbsorptionCoefficients;
		DPROPERTY()
		FMaterialExpressionInput PhaseG;
		DPROPERTY()
		FMaterialExpressionInput ColorScaleBehindWater;

		auto GetAuthoredInputCount() const -> uint32 override { return 4; }
		auto Build(MIR::FEmitter&) const -> void override {}
	};
}

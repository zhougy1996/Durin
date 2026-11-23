#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialCustomOutput.h"

#include "MaterialExpressionSingleLayerWaterOutput.gen.h"

namespace Durin
{
	// Material output terminal. Its inputs become independent pixel roots.
	DCLASS()
	class DMaterialExpressionSingleLayerWaterOutput : public DMaterialExpressionCustomOutput
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

		auto GetFunctionName() const -> std::string_view override { return SingleLayerWater::GetOutputDefinition().FunctionName; }
		auto GetDisplayName() const -> std::string_view override { return SingleLayerWater::GetOutputDefinition().DisplayName; }
		auto GetNumOutputs() const -> int32 override { return static_cast<int32>(SingleLayerWater::GetOutputDefinition().Outputs.size()); }
		auto GetAuthoredInputCount() const -> uint32 override { return 4; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}

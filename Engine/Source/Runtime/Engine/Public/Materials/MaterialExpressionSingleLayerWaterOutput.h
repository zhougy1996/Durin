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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::SingleLayerWaterOutput, .Name = GetDisplayName(),
				.Description = "Sets the pixel inputs for the Single Layer Water shading model.",
				.Category = "Surface", .SearchKeywords = "water refraction volume"};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			const auto Slots = SingleLayerWater::GetOutputDefinition().Outputs;
			if (Index < 0 || static_cast<size_t>(Index) >= Slots.size()) return {};
			return {Slots[Index].Name, Slots[Index].Semantics.Type};
		}
		auto GetFunctionName() const -> std::string_view override { return SingleLayerWater::GetOutputDefinition().FunctionName; }
		auto GetDisplayName() const -> std::string_view override { return SingleLayerWater::GetOutputDefinition().DisplayName; }
		auto GetNumOutputs() const -> int32 override { return static_cast<int32>(SingleLayerWater::GetOutputDefinition().Outputs.size()); }
		auto GetAuthoredInputCount() const -> uint32 override { return static_cast<uint32>(GetNumOutputs()); }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}

#include "Materials/MaterialCustomOutput.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialExpressionSingleLayerWaterOutput.h"

namespace Durin
{
	auto SingleLayerWater::GetOutputDefinition() -> const FMaterialCustomOutputDefinition&
	{
		using T = EMaterialProgramValueType;
		constexpr auto Pixel = EMaterialEvaluationStage::Pixel;
		static constexpr std::array Slots{
			FMaterialCustomOutputSlot{Scattering, "Scattering Coefficients", {T::Float3, Pixel}, {}},
			FMaterialCustomOutputSlot{Absorption, "Absorption Coefficients", {T::Float3, Pixel}, {}},
			FMaterialCustomOutputSlot{PhaseG, "Phase G", {T::Float, Pixel}, {}},
			FMaterialCustomOutputSlot{ColorScale, "Color Scale Behind Water", {T::Float3, Pixel}, {1,1,1}}
		};
		static constexpr FMaterialCustomOutputDefinition Definition{"GetSingleLayerWaterMaterialOutput",
			"Single Layer Water Material", EMaterialShadingModel::SingleLayerWater, EMaterialShaderEntry::SingleLayerWater, Slots};
		return Definition;
	}
	auto DMaterialExpressionSingleLayerWaterOutput::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (Emitter.IsCompilingMaterial() && !ScatteringCoefficients.IsConnected() && !AbsorptionCoefficients.IsConnected() && !PhaseG.IsConnected())
			return Emitter.Fail("No inputs to Single Layer Water Material.");
		const std::array Pins{&ScatteringCoefficients, &AbsorptionCoefficients, &PhaseG, &ColorScaleBehindWater};
		std::array<std::optional<MIR::FValue>, 4> Args;
		for (size_t Index = 0; Index < Pins.size(); ++Index)
		{
			const auto& Pin = *Pins[Index];
			if (Pin.IsConnected()) Args[Index] = Emitter.Resolve(Pin);
			else if (Pin.OutputIndex != 0) return Emitter.Fail("Disconnected water input has an output selector.");
		}
		Emitter.SetCustomOutputs(GetFunctionName(), Args, EMaterialEvaluationStage::Pixel);
	}
	auto GetMaterialCustomOutputs() -> std::span<const FMaterialCustomOutputDefinition>
	{
		static const std::array Definitions{SingleLayerWater::GetOutputDefinition()};
		return Definitions;
	}
	auto FindMaterialCustomOutputSlot(FGuid Id) -> const FMaterialCustomOutputSlot*
	{
		for (const auto& Family : GetMaterialCustomOutputs())
			for (const auto& Slot : Family.Outputs)
				if (Slot.Id == Id) return &Slot;
		return nullptr;
	}

	auto MIR::FModule::GetCustomOutput(FGuid Id) -> FAttributeRoot&
	{
		auto It = std::ranges::find(CustomOutputRoots, Id, &FAttributeRoot::AttributeID);
		if (It != CustomOutputRoots.end()) return *It;
		const auto* Slot = FindMaterialCustomOutputSlot(Id);
		check(Slot);
		return CustomOutputRoots.emplace_back(FAttributeRoot{.AttributeID = Id, .Type = Slot->Semantics.Type,
			.LegalStages = Slot->Semantics.Stages, .SpatialKind = Slot->Semantics.Kind,
			.CoordinateSpace = Slot->Semantics.Space, .Literal = Slot->DefaultValue});
	}
	auto MIR::FModule::GetCustomOutput(FGuid Id) const -> const FAttributeRoot&
	{
		const auto It = std::ranges::find(CustomOutputRoots, Id, &FAttributeRoot::AttributeID);
		check(It != CustomOutputRoots.end());
		return *It;
	}
}

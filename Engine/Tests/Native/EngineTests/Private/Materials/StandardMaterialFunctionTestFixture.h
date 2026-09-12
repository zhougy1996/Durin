#pragma once

#include "AssetForge/Builtins/StandardMaterialFunctions.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"

namespace Durin::Testing
{
	// Transient ordinary functions, with the same authored graph definitions as shipped assets.
	inline auto SetStandardMaterialProgramForTest(DMaterial& Material) -> bool
	{
		using namespace AssetForge::Builtins;
		FStandardMaterialFunctions Functions;
		const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM,
			&Functions.StandardPBR, &Functions.StandardPBR_ORM};
		for (uint32 I = 0; I < Slots.size(); ++I)
		{
			auto* Function = NewObject<DMaterialFunction>(&Material, FName(std::format("StandardFunction{}", I + 1)));
			if (!Function || !Function->SetFunctionGraph(MakeStandardMaterialFunctionGraph(
				static_cast<EStandardMaterialFunction>(I + 1), Functions))) return false;
			*Slots[I] = Function;
		}
		std::vector<FMaterialFunctionCall> Calls;
		FMaterialGraphPresentation Presentation;
		auto Program = MakeImportedSurfaceFunctionProgram(Functions, Calls, Presentation);
		if (!Material.SetMaterialProgramAndFunctionCalls(std::move(Program), std::move(Calls))) return false;
		return Material.SetMaterialGraphPresentation(std::move(Presentation));
	}
}

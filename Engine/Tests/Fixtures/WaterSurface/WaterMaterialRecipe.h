#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin::WaterMaterialRecipe
{
	auto BuildInput() -> MIR::FCompilerInput;
}

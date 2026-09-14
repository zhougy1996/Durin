#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin::AssetForge::Builtins
{
	// Explicit Metallic/Roughness template with independent map, factor, and UV
	// parameters. Construction is pure; callers choose where and when to save it.
	ASSETFORGEBUILTINS_API auto MakePBRSurfaceMaterialMRProgram(
		FMaterialGraphPresentation& OutPresentation) -> FMaterialProgram;
}

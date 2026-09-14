#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/Builtins/MaterialExpressionRecipe.h"

namespace Durin::AssetForge::Builtins
{
	// Explicit Metallic/Roughness template with independent map, factor, and UV
	// parameters. Construction retains transient expressions; callers choose where and when to save it.
	ASSETFORGEBUILTINS_API auto MakePBRSurfaceMaterialMRExpressions() -> FMaterialExpressionRecipe;
}

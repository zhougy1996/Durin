#pragma once

#include "StaticMeshBuildAPI.h"
#include "StaticMesh/StaticMeshBuildProvider.h"

namespace Durin
{
	// Implements detached render and collision recipes without asset access.
	class FStaticMeshBuildOperations
	{
	public:
		STATICMESHBUILD_API static auto BuildRenderRecipe(
			const FStaticMeshRecipeBuildRequest& Request,
			FStaticMeshRecipeBuildProduct& OutProduct,
			std::string& OutError) -> bool;
		STATICMESHBUILD_API static auto BuildCollisionRecipe(
			const FStaticMeshCollisionRecipeRequest& Request,
			FStaticMeshCollisionRecipeProduct& OutProduct,
			std::string& OutError) -> bool;
	};
}

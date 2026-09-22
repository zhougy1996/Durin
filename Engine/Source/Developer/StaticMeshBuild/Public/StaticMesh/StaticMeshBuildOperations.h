#pragma once

#include <expected>

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
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError>;
		STATICMESHBUILD_API static auto BuildCollisionRecipe(
			const FStaticMeshCollisionRecipeRequest& Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshCollisionRecipeProduct, FStaticMeshRecipeError>;
	};
}

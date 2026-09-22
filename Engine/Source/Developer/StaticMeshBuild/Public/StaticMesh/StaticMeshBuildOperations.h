#pragma once

#include <expected>

#include "StaticMeshBuildAPI.h"
#include "StaticMesh/StaticMeshBuildProvider.h"

namespace Durin
{
	// Implements detached render recipes without asset access.
	class FStaticMeshBuildOperations
	{
	public:
		STATICMESHBUILD_API static auto BuildRenderRecipe(
			const FStaticMeshRecipeBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {}) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError>;

	};
}

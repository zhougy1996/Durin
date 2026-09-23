#pragma once

#include <expected>

#include "StaticMesh/StaticMeshBuildTypes.h"

namespace Durin
{
	// Builds detached CPU render data without asset access.
	class FStaticMeshBuilder
	{
	public:
		static auto Build(
			const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {}) -> std::expected<FStaticMeshRenderBuildProduct, FStaticMeshRenderBuildError>;
	};
}

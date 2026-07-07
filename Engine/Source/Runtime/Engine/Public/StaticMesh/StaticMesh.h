#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;

	class DStaticMesh
	{
	public:
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		ENGINE_API auto GetRenderData() -> FStaticMeshRenderData*;

		ENGINE_API auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;

		ENGINE_API static auto CreateDebugTriangle() -> std::shared_ptr<DStaticMesh>;

	private:
		std::unique_ptr<FStaticMeshRenderData> RenderData;
	};
}

#pragma once

#include "Engine/API.h"

namespace Doge
{
	struct FStaticMeshRenderData;

	class DStaticMesh
	{
	public:
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;

		ENGINE_API auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;

	private:
		std::unique_ptr<FStaticMeshRenderData> RenderData;
	};
}
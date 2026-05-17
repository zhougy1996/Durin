#include "StaticMesh/StaticMesh.h"

#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void
	{
		RenderData = std::move(InRenderData);
	}
}

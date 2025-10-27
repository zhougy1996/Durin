#include "StaticMesh/StaticMesh.h"

#include "StaticMesh/StaticMeshResources.h"


auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
{
	return RenderData.get();
}

auto DStaticMesh::SetRenderData(TUniquePtr<FStaticMeshRenderData> InRenderData) -> void
{
	RenderData = std::move(InRenderData);
}

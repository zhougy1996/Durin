#include "StaticMesh/StaticMesh.h"

#include "StaticMesh/StaticMeshResources.h"

DStaticMesh::DStaticMesh()
{
}

DStaticMesh::~DStaticMesh()
{
}

auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
{
	return RenderData_.get();
}

auto DStaticMesh::SetRenderData(TUniquePtr<FStaticMeshRenderData> InRenderData) -> void
{
	RenderData_ = std::move(InRenderData);
}

#pragma once

struct FStaticMeshRenderData;

class DStaticMesh
{
public:
	ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;

	ENGINE_API auto SetRenderData(TUniquePtr<FStaticMeshRenderData> InRenderData) -> void;

private:
	TUniquePtr<FStaticMeshRenderData> RenderData;
};
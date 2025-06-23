#pragma once

class FStaticMeshRenderData;

class DStaticMesh
{

private:
	TUniquePtr<FStaticMeshRenderData> RenderData_;
};
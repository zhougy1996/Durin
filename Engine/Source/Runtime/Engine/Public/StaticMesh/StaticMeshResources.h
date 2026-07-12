#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"

#include "RHIResources.h"

namespace Durin
{
	struct FStaticMeshBuildData
	{
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
	};

	class FRHICommandListImmediate;

	struct FStaticMeshRenderData
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<uint32> Indices;
		FBox LocalBounds;
		FBufferRHIRef PositionVertexBufferRHI;
		FBufferRHIRef NormalVertexBufferRHI;
		FBufferRHIRef IndexBufferRHI;
		uint32 IndexCount = 0;

		ENGINE_API auto InitResources(FRHICommandListImmediate& RHICmdList) -> void;
		ENGINE_API auto ReleaseResources() -> void;
		ENGINE_API auto IsReadyForRendering() const -> bool;
		ENGINE_API auto RecalculateBounds() -> void;
	};
}

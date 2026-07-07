#pragma once

#include "EngineAPI.h"

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
		std::vector<uint32> Indices;
		FBufferRHIRef PositionVertexBufferRHI;
		FBufferRHIRef IndexBufferRHI;
		uint32 IndexCount = 0;

		ENGINE_API auto InitResources(FRHICommandListImmediate& RHICmdList) -> void;
		ENGINE_API auto ReleaseResources() -> void;
		ENGINE_API auto IsReadyForRendering() const -> bool;
	};
}

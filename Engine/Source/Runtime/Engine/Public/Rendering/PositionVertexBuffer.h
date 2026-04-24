#pragma once

#include "EngineAPI.h"

#include "RenderResource.h"

namespace Doge
{
	class FPositionVertexData;

	struct FPositionVertex
	{
		FVector3f Position;
	};

	class FPositionVertexBuffer : public FVertexBuffer
	{
	public:
		/** Default constructor. */
		ENGINE_API FPositionVertexBuffer();

		/** Destructor. */
		ENGINE_API ~FPositionVertexBuffer() override;

		ENGINE_API void CleanUp();

		ENGINE_API void Init(uint32 InNumVertices, bool bInNeedsCPUAccess = true);

		ENGINE_API void Init(const std::vector<FVector3f>& InPositions, bool bInNeedsCPUAccess = true);

		// FRenderResource interface.
		ENGINE_API void InitRHI(FRHICommandListBase& RHICmdList) override;

		ENGINE_API void ReleaseRHI() override;

	private:
		std::shared_ptr<FRHIBuffer> CreateRHIBuffer(FRHICommandListBase& RHICmdList);

		/** Allocates the vertex data storage type. */
		void AllocateData(bool bInNeedsCPUAccess = true);

		FPositionVertexData* VertexData;

		/** The cached vertex data pointer. */
		uint8* Data;

		/** The cached vertex stride. */
		uint32 Stride;

		/** The cached number of vertices. */
		uint32 NumVertices;

		bool bNeedsCPUAccess = true;
	};
}
#pragma once

#include "RenderResource.h"

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
	ENGINE_API ~FPositionVertexBuffer();

	ENGINE_API void CleanUp();

	ENGINE_API void Init(uint32 NumVertices, bool bInNeedsCPUAccess = true);

	ENGINE_API void Init(const std::vector<FVector3f>& InPositions, bool bInNeedsCPUAccess = true);

	// FRenderResource interface.
	ENGINE_API virtual void InitRHI(FRHICommandList& RHICmdList) override;

	ENGINE_API virtual void ReleaseRHI() override;

private:
	TSharedPtr<FRHIBuffer> CreateRHIBuffer(FRHICommandList& RHICmdList);

	/** Allocates the vertex data storage type. */
	void AllocateData(bool bInNeedsCPUAccess = true);

	FPositionVertexData* VertexData_;

	/** The cached vertex data pointer. */
	uint8* Data_;

	/** The cached vertex stride. */
	uint32 Stride_;

	/** The cached number of vertices. */
	uint32 NumVertices_;

	bool bNeedsCPUAccess_ = true;
};
#include "RenderResource.h"

namespace Durin
{
	void FRenderResource::ReleaseRHIForAllResources()
	{
	}

	void FRenderResource::InitPreRHIResources()
	{
	}

	FRenderResource::FRenderResource()
	{
	}

	FRenderResource::FRenderResource(ERHIFeatureLevel InFeatureLevel)
	{
	}

	FRenderResource::~FRenderResource()
	{
		if (IsInitialized())
		{
			DURIN_ERROR("A FRenderResource was not released before destruction.");
		}
	}
	void FRenderResource::InitRHI(FRHICommandListBase& RHICmdList) {}

	void FRenderResource::InitResource(FRHICommandListBase& RHICmdList)
	{
	}

	void FRenderResource::ReleaseResource()
	{
	}

	void FRenderResource::UpdateRHI(FRHICommandListBase& RHICmdList)
	{
	}

	FVertexBuffer::FVertexBuffer() = default;
	FVertexBuffer::~FVertexBuffer() = default;

	void FVertexBuffer::ReleaseRHI()
	{
	}

	void FVertexBuffer::SetRHI(const std::shared_ptr<FRHIBuffer>& BufferRHI)
	{
	}
}


#include "RenderResource.h"

namespace Doge
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
			DOGE_ERROR("A FRenderResource was not released before destruction.");
		}
	}
	void FRenderResource::InitRHI(FRHICommandList& RHICmdList) {}

	void FRenderResource::InitResource(FRHICommandList& RHICmdList)
	{
	}

	void FRenderResource::ReleaseResource()
	{
	}

	void FRenderResource::UpdateRHI(FRHICommandList& RHICmdList)
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


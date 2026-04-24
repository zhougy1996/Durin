#include "Rendering/PositionVertexBuffer.h"

#include "StaticMesh/StaticMeshVertexData.h"

namespace Doge
{
	class FPositionVertexData : public TStaticMeshVertexData<FPositionVertex>
	{
	public:
		FPositionVertexData(bool InNeedsCPUAccess = false)
			: TStaticMeshVertexData<FPositionVertex>(InNeedsCPUAccess)
		{
		}
	};
	FPositionVertexBuffer::FPositionVertexBuffer()
		: VertexData(nullptr)
		, Data(nullptr)
		, Stride(0)
		, NumVertices(0)
	{
	}

	FPositionVertexBuffer::~FPositionVertexBuffer()
	{
	}

	void FPositionVertexBuffer::CleanUp()
	{
		if (VertexData)
		{
			delete VertexData;
			VertexData = nullptr;
		}
	}

	void FPositionVertexBuffer::Init(uint32 InNumVertices, bool bInNeedsCPUAccess)
	{
		NumVertices = InNumVertices;
		bNeedsCPUAccess = bInNeedsCPUAccess;

		// Allocate the vertex data storage type.
		AllocateData(bInNeedsCPUAccess);

		// Allocate the vertex data buffer.
		VertexData->ResizeBuffer(NumVertices);
		Data = NumVertices ? VertexData->GetDataPointer() : nullptr;
	}

	void FPositionVertexBuffer::Init(const std::vector<FVector3f>& InPositions, bool bInNeedsCPUAccess)
	{
		NumVertices = static_cast<uint32>(InPositions.size());
		bNeedsCPUAccess = bInNeedsCPUAccess;
		if (NumVertices)
		{
			AllocateData(bInNeedsCPUAccess);
			check(Stride == sizeof(FVector3f));
			VertexData->ResizeBuffer(NumVertices);
			Data = VertexData->GetDataPointer();
			memcpy(Data, InPositions.data(), Stride * NumVertices);
		}
	}

	void FPositionVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
	{
	}

	void FPositionVertexBuffer::ReleaseRHI()
	{
	}

	std::shared_ptr<FRHIBuffer> FPositionVertexBuffer::CreateRHIBuffer(FRHICommandListBase& RHICmdList)
	{
		return FRenderResource::CreateRHIBuffer(RHICmdList, VertexData, NumVertices, EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource, STR("FPositionVertexBuffer"));
	}

	void FPositionVertexBuffer::AllocateData(bool bInNeedsCPUAccess)
	{
		CleanUp();

		VertexData = new FPositionVertexData(bInNeedsCPUAccess);

		Stride = VertexData->GetStride();

		// NumVertices do not need to be set here, as it will be set when ResizeBuffer is called.
	}
}
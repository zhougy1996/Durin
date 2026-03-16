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
		: VertexData_(nullptr)
		, Data_(nullptr)
		, Stride_(0)
		, NumVertices_(0)
	{
	}

	FPositionVertexBuffer::~FPositionVertexBuffer()
	{
	}

	void FPositionVertexBuffer::CleanUp()
	{
		if (VertexData_)
		{
			delete VertexData_;
			VertexData_ = nullptr;
		}
	}

	void FPositionVertexBuffer::Init(uint32 InNumVertices, bool bInNeedsCPUAccess)
	{
		NumVertices_ = InNumVertices;
		bNeedsCPUAccess_ = bInNeedsCPUAccess;

		// Allocate the vertex data storage type.
		AllocateData(bInNeedsCPUAccess);

		// Allocate the vertex data buffer.
		VertexData_->ResizeBuffer(NumVertices_);
		Data_ = NumVertices_ ? VertexData_->GetDataPointer() : nullptr;
	}

	void FPositionVertexBuffer::Init(const std::vector<FVector3f>& InPositions, bool bInNeedsCPUAccess)
	{
		NumVertices_ = static_cast<uint32>(InPositions.size());
		bNeedsCPUAccess_ = bInNeedsCPUAccess;
		if (NumVertices_)
		{
			AllocateData(bInNeedsCPUAccess);
			check(Stride_ == sizeof(FVector3f));
			VertexData_->ResizeBuffer(NumVertices_);
			Data_ = VertexData_->GetDataPointer();
			memcpy(Data_, InPositions.data(), Stride_ * NumVertices_);
		}
	}

	void FPositionVertexBuffer::InitRHI(FRHICommandList& RHICmdList)
	{
	}

	void FPositionVertexBuffer::ReleaseRHI()
	{
	}

	std::shared_ptr<FRHIBuffer> FPositionVertexBuffer::CreateRHIBuffer(FRHICommandList& RHICmdList)
	{
		return FRenderResource::CreateRHIBuffer(RHICmdList, VertexData_, NumVertices_, BUF_Static | BUF_ShaderResource, STR("FPositionVertexBuffer"));
	}

	void FPositionVertexBuffer::AllocateData(bool bInNeedsCPUAccess)
	{
		CleanUp();

		VertexData_ = new FPositionVertexData(bInNeedsCPUAccess);

		Stride_ = VertexData_->GetStride();

		// NumVertices do not need to be set here, as it will be set when ResizeBuffer is called.
	}
}
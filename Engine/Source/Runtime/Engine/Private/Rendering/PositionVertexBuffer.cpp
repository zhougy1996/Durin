#include "Rendering/PositionVertexBuffer.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"

namespace Durin
{
	FPositionVertexBuffer::FPositionVertexBuffer() = default;
	FPositionVertexBuffer::~FPositionVertexBuffer() = default;

	auto FPositionVertexBuffer::Init(uint32 NumVertices, bool bInNeedsCPUAccess) -> void
	{
		Init(std::vector<FVector3f>(NumVertices), bInNeedsCPUAccess);
	}

	auto FPositionVertexBuffer::Init(
		const std::vector<FVector3f>& InPositions,
		bool bInNeedsCPUAccess) -> void
	{
		Init(std::vector<FVector3f>(InPositions), bInNeedsCPUAccess);
	}

	auto FPositionVertexBuffer::Init(
		std::vector<FVector3f>&& InPositions,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		bNeedsCPUAccess = bInNeedsCPUAccess;
		SharedPositions = {};
		Positions = std::move(InPositions);
		NumVertices = static_cast<uint32>(Positions.size());
	}

	auto FPositionVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		const auto Positions = GetPositions();
		if (Positions.empty() || GetRHI() != nullptr) return;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshPositionVertexBuffer",
			static_cast<uint32>(Positions.size() * sizeof(FVector3f)));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = Positions.data();
		Desc.InitialData.Size =
			static_cast<uint32>(Positions.size() * sizeof(FVector3f));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
		// Buffer creation snapshots InitialData before returning, including deferred uploads.
		// Keep the source on failure so creation can be retried.
		if (GetRHI() != nullptr && !bNeedsCPUAccess)
		{
			std::vector<FVector3f>().swap(this->Positions);
			SharedPositions = {};
		}
	}
}

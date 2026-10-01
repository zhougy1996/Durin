#include "Rendering/PositionVertexBuffer.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"

namespace Durin
{
	FPositionVertexBuffer::FPositionVertexBuffer() = default;
	FPositionVertexBuffer::~FPositionVertexBuffer() = default;

	auto FPositionVertexBuffer::Init(
		std::vector<FVector3f> InPositions,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		bNeedsCPUAccess = bInNeedsCPUAccess;
		SharedPositions = {};
		Positions = std::move(InPositions);
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
	}
}

#include "StaticMesh/StaticMesh.h"

#include "StaticMesh/StaticMeshResources.h"

#include "RHICommandList.h"

namespace Durin
{
	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetRenderData() -> FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void
	{
		RenderData = std::move(InRenderData);
	}

	auto DStaticMesh::CreateDebugTriangle() -> std::shared_ptr<DStaticMesh>
	{
		auto Mesh = std::make_shared<DStaticMesh>();
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->Positions = {
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		};
		RenderData->Indices = {0, 1, 2};
		RenderData->IndexCount = static_cast<uint32>(RenderData->Indices.size());
		Mesh->SetRenderData(std::move(RenderData));
		return Mesh;
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> void
	{
		if (PositionVertexBufferRHI == nullptr && !Positions.empty())
		{
			FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex(
				"StaticMeshPositionVertexBuffer",
				static_cast<uint32>(Positions.size() * sizeof(FVector3f))
			);
			VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
			VertexBufferDesc.InitialData.Data = Positions.data();
			VertexBufferDesc.InitialData.Size = static_cast<uint32>(Positions.size() * sizeof(FVector3f));
			PositionVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, VertexBufferDesc);
		}

		if (IndexBufferRHI == nullptr && !Indices.empty())
		{
			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
				"StaticMeshIndexBuffer",
				static_cast<uint32>(Indices.size() * sizeof(uint32)),
				sizeof(uint32)
			);
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData.Data = Indices.data();
			IndexBufferDesc.InitialData.Size = static_cast<uint32>(Indices.size() * sizeof(uint32));
			IndexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, IndexBufferDesc);
		}

		IndexCount = static_cast<uint32>(Indices.size());
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		PositionVertexBufferRHI = nullptr;
		IndexBufferRHI = nullptr;
	}

	auto FStaticMeshRenderData::IsReadyForRendering() const -> bool
	{
		return PositionVertexBufferRHI != nullptr && IndexBufferRHI != nullptr && IndexCount > 0;
	}
}

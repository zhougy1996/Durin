#include "RendererFullscreenGeometry.h"

#include "RHI.h"
#include "RHICommandList.h"

namespace Durin::RendererFullscreenGeometry
{
	namespace
	{
		struct FState
		{
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			bool bCreateAttempted = false;
		};

		FState GState;
	}

	auto EnsureResources(FRHICommandListImmediate& CommandList) -> bool
	{
		if (GState.bCreateAttempted)
			return GState.VertexBuffer != nullptr && GState.IndexBuffer != nullptr;
		GState.bCreateAttempted = true;

		const std::array<FVertex, 3> Vertices = {
			FVertex{FVector2f{-1.0f, -1.0f}, FVector2f{0.0f, 0.0f}},
			FVertex{FVector2f{3.0f, -1.0f}, FVector2f{2.0f, 0.0f}},
			FVertex{FVector2f{-1.0f, 3.0f}, FVector2f{0.0f, 2.0f}},
		};
		const std::array<uint32, 3> Indices = {0, 1, 2};

		FRHIBufferCreateDesc VertexDesc = FRHIBufferCreateDesc::CreateVertex(
			"RendererFullscreenVertexBuffer",
			sizeof(FVertex) * static_cast<uint32>(Vertices.size()));
		VertexDesc.Usage |= EBufferUsageFlags::Static;
		VertexDesc.InitialData = {
			Vertices.data(),
			static_cast<uint32>(sizeof(FVertex) * Vertices.size())};
		FBufferRHIRef VertexBuffer =
			GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);

		FRHIBufferCreateDesc IndexDesc = FRHIBufferCreateDesc::CreateIndex(
			"RendererFullscreenIndexBuffer",
			sizeof(uint32) * static_cast<uint32>(Indices.size()),
			sizeof(uint32));
		IndexDesc.Usage |= EBufferUsageFlags::Static;
		IndexDesc.InitialData = {
			Indices.data(),
			static_cast<uint32>(sizeof(uint32) * Indices.size())};
		FBufferRHIRef IndexBuffer =
			GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
		if (VertexBuffer == nullptr || IndexBuffer == nullptr)
			return false;

		GState.VertexBuffer = std::move(VertexBuffer);
		GState.IndexBuffer = std::move(IndexBuffer);
		return true;
	}

	auto GetVertexBuffer() -> const FBufferRHIRef&
	{
		return GState.VertexBuffer;
	}

	auto GetIndexBuffer() -> const FBufferRHIRef&
	{
		return GState.IndexBuffer;
	}

	auto ReleaseResources() -> void
	{
		GState = {};
	}
} // namespace Durin::RendererFullscreenGeometry

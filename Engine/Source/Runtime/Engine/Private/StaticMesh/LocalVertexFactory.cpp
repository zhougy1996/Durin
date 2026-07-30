#include "StaticMesh/LocalVertexFactory.h"

#include "RenderingThread.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto FLocalVertexFactory::SetData(
		const FStaticMeshVertexBuffers& VertexBuffers) -> bool
	{
		check(!IsInitialized());
		const auto& PositionBuffer = VertexBuffers.PositionVertexBuffer;
		const auto& StaticMeshBuffer =
			VertexBuffers.StaticMeshVertexBuffer;
		const uint32 NumVertices = PositionBuffer.GetNumVertices();
		if (NumVertices == 0
			|| StaticMeshBuffer.GetNumVertices() != NumVertices
			|| StaticMeshBuffer.TangentsVertexBuffer.GetNumVertices()
				!= NumVertices
			|| StaticMeshBuffer.TexCoordVertexBuffer.GetNumVertices()
				!= NumVertices
			|| VertexBuffers.ColorVertexBuffer.GetNumVertices()
				!= NumVertices)
		{
			Data = {};
			return false;
		}

		constexpr uint16 PositionStride = sizeof(FVector3f);
		constexpr uint16 AttributeStride =
			sizeof(FStaticMeshPackedVertex);
		Data.PositionComponent = {
			.VertexBuffer = &PositionBuffer,
			.Offset = 0,
			.Stride = PositionStride,
			.Type = EVertexElementType::Float3,
			.AttributeIndex = 0,
			.StreamIndex = 0};
		Data.TangentBasisComponents[0] = {
			.VertexBuffer = &StaticMeshBuffer,
			.Offset = offsetof(FStaticMeshPackedVertex, Normal),
			.Stride = AttributeStride,
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 1,
			.StreamIndex = 1};
		Data.TangentBasisComponents[1] = {
			.VertexBuffer = &StaticMeshBuffer,
			.Offset = offsetof(FStaticMeshPackedVertex, Tangent),
			.Stride = AttributeStride,
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 2,
			.StreamIndex = 1};
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
		{
			Data.TextureCoordinates[Channel] = {
				.VertexBuffer = &StaticMeshBuffer,
				.Offset = offsetof(FStaticMeshPackedVertex, TexCoords)
					+ sizeof(FVector2f) * Channel,
				.Stride = AttributeStride,
				.Type = EVertexElementType::Float2,
				.AttributeIndex = static_cast<uint8>(3 + Channel),
				.StreamIndex = 1};
		}
		Data.ColorComponent = {
			.VertexBuffer = &StaticMeshBuffer,
			.Offset = offsetof(FStaticMeshPackedVertex, Color),
			.Stride = AttributeStride,
			.Type = EVertexElementType::UByte4N,
			.AttributeIndex = 7,
			.StreamIndex = 1};
		Data.NumVertices = NumVertices;
		return true;
	}

	auto FLocalVertexFactory::GetDeclarationElements() const
		-> FVertexDeclarationElementList
	{
		FVertexDeclarationElementList Elements{};
		if (Data.NumVertices == 0
			|| !Data.PositionComponent.IsValid()
			|| !Data.ColorComponent.IsValid()
			|| !std::ranges::all_of(
				Data.TangentBasisComponents,
				[](const FVertexStreamComponent& Component) {
					return Component.IsValid();
				})
			|| !std::ranges::all_of(
				Data.TextureCoordinates,
				[](const FVertexStreamComponent& Component) {
					return Component.IsValid();
				}))
		{
			return Elements;
		}
		Elements[0] = Data.PositionComponent.ToVertexElement();
		Elements[1] = Data.TangentBasisComponents[0].ToVertexElement();
		Elements[2] = Data.TangentBasisComponents[1].ToVertexElement();
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
		{
			Elements[3 + Channel] =
				Data.TextureCoordinates[Channel].ToVertexElement();
		}
		Elements[7] = Data.ColorComponent.ToVertexElement();
		return Elements;
	}

	auto FLocalVertexFactory::IsDataValid() const -> bool
	{
		if (Data.NumVertices == 0
			|| !Data.PositionComponent.IsValid()
			|| !Data.ColorComponent.IsValid())
		{
			return false;
		}
		if (!std::ranges::all_of(
				Data.TangentBasisComponents,
				[](const FVertexStreamComponent& Component) {
					return Component.IsValid();
				})
			|| !std::ranges::all_of(
				Data.TextureCoordinates,
				[](const FVertexStreamComponent& Component) {
					return Component.IsValid();
				}))
		{
			return false;
		}

		const FVertexBuffer* PositionBuffer =
			Data.PositionComponent.VertexBuffer;
		const FVertexBuffer* AttributeBuffer =
			Data.TangentBasisComponents[0].VertexBuffer;
		return PositionBuffer->GetRHI() != nullptr
			&& AttributeBuffer->GetRHI() != nullptr
			&& Data.TangentBasisComponents[1].VertexBuffer
				== AttributeBuffer
			&& std::ranges::all_of(
				Data.TextureCoordinates,
				[AttributeBuffer](
					const FVertexStreamComponent& Component) {
					return Component.VertexBuffer == AttributeBuffer;
				})
			&& Data.ColorComponent.VertexBuffer == AttributeBuffer;
	}

	auto FLocalVertexFactory::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		if (!IsDataValid()) return;
		SetDeclarationElements(GetDeclarationElements());
		SetStreams({
			{
				.StreamIndex = Data.PositionComponent.StreamIndex,
				.VertexBuffer =
					Data.PositionComponent.VertexBuffer->GetRHI(),
				.Offset = 0,
				.Stride = Data.PositionComponent.Stride,
			},
			{
				.StreamIndex =
					Data.TangentBasisComponents[0].StreamIndex,
				.VertexBuffer =
					Data.TangentBasisComponents[0].VertexBuffer->GetRHI(),
				.Offset = 0,
				.Stride = Data.TangentBasisComponents[0].Stride,
			}});
		FVertexFactory::InitRHI(RHICmdList);
	}
}

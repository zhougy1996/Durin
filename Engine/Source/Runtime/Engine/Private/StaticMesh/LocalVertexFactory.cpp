#include "StaticMesh/LocalVertexFactory.h"

#include "RHICommandList.h"
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

		const auto& TangentsBuffer =
			StaticMeshBuffer.TangentsVertexBuffer;
		const auto& TexCoordBuffer =
			StaticMeshBuffer.TexCoordVertexBuffer;
		const auto& ColorBuffer =
			VertexBuffers.ColorVertexBuffer;
		Data.PositionComponent = {
			.VertexBuffer = &PositionBuffer,
			.Offset = 0,
			.Stride = static_cast<uint16>(PositionBuffer.GetStride()),
			.Type = EVertexElementType::Float3,
			.AttributeIndex = 0,
			.StreamIndex = 0};
		Data.TangentBasisComponents[0] = {
			.VertexBuffer = &TangentsBuffer,
			.Offset = offsetof(FStaticMeshPackedTangentBasis, Normal),
			.Stride = static_cast<uint16>(TangentsBuffer.GetStride()),
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 1,
			.StreamIndex = 1};
		Data.TangentBasisComponents[1] = {
			.VertexBuffer = &TangentsBuffer,
			.Offset = offsetof(FStaticMeshPackedTangentBasis, Tangent),
			.Stride = static_cast<uint16>(TangentsBuffer.GetStride()),
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 2,
			.StreamIndex = 1};
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
		{
			Data.TextureCoordinates[Channel] = {
				.VertexBuffer = &TexCoordBuffer,
				.Offset = static_cast<uint32>(offsetof(FStaticMeshTexcoordVertex, TexCoords)
					+ sizeof(FVector2f) * Channel),
				.Stride =
					static_cast<uint16>(TexCoordBuffer.GetStride()),
				.Type = EVertexElementType::Float2,
				.AttributeIndex = static_cast<uint8>(3 + Channel),
				.StreamIndex = 2};
		}
		Data.ColorComponent = {
			.VertexBuffer = &ColorBuffer,
			.Offset = offsetof(FStaticMeshColorVertex, Color),
			.Stride = static_cast<uint16>(ColorBuffer.GetStride()),
			.Type = EVertexElementType::UByte4N,
			.AttributeIndex = 7,
			.StreamIndex = 3};
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
		const FVertexBuffer* TangentsBuffer =
			Data.TangentBasisComponents[0].VertexBuffer;
		const FVertexBuffer* TexCoordBuffer =
			Data.TextureCoordinates[0].VertexBuffer;
		const FVertexBuffer* ColorBuffer =
			Data.ColorComponent.VertexBuffer;
		return PositionBuffer->GetRHI() != nullptr
			&& TangentsBuffer->GetRHI() != nullptr
			&& TexCoordBuffer->GetRHI() != nullptr
			&& ColorBuffer->GetRHI() != nullptr
			&& Data.TangentBasisComponents[1].VertexBuffer
				== TangentsBuffer
			&& std::ranges::all_of(
				Data.TextureCoordinates,
				[TexCoordBuffer](
					const FVertexStreamComponent& Component) {
					return Component.VertexBuffer == TexCoordBuffer;
				})
			&& PositionBuffer != TangentsBuffer
			&& PositionBuffer != TexCoordBuffer
			&& PositionBuffer != ColorBuffer
			&& TangentsBuffer != TexCoordBuffer
			&& TangentsBuffer != ColorBuffer
			&& TexCoordBuffer != ColorBuffer;
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
			},
			{
				.StreamIndex =
					Data.TextureCoordinates[0].StreamIndex,
				.VertexBuffer =
					Data.TextureCoordinates[0].VertexBuffer->GetRHI(),
				.Offset = 0,
				.Stride = Data.TextureCoordinates[0].Stride,
			},
			{
				.StreamIndex = Data.ColorComponent.StreamIndex,
				.VertexBuffer =
					Data.ColorComponent.VertexBuffer->GetRHI(),
				.Offset = 0,
				.Stride = Data.ColorComponent.Stride,
			}});
		FVertexFactory::InitRHI(RHICmdList);
	}

	auto FLocalVertexFactory::BindPositionStream(
		FRHICommandListImmediate& CommandList) const -> void
	{
		check(IsInRenderingThread());
		check(Data.PositionComponent.IsValid());
		check(Data.PositionComponent.VertexBuffer->GetRHI() != nullptr);
		CommandList.BindVertexBuffer(
			Data.PositionComponent.StreamIndex,
			Data.PositionComponent.VertexBuffer->GetRHI(),
			0);
	}
}

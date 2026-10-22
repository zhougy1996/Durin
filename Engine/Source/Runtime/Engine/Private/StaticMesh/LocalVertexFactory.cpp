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

		FVertexDeclarationElementList Elements{};
		GetPSOPrecacheVertexFetchElements(EVertexInputStreamType::Default, Elements);
		const auto Component = [](const FVertexElement& Element, const FVertexBuffer& Buffer) {
			return FVertexStreamComponent{&Buffer, Element.Offset, Element.Stride, Element.Type,
				Element.AttributeIndex, Element.StreamIndex};
		};
		Data.PositionComponent = Component(Elements[0], PositionBuffer);
		Data.TangentBasisComponents[0] = Component(Elements[1], StaticMeshBuffer.TangentsVertexBuffer);
		Data.TangentBasisComponents[1] = Component(Elements[2], StaticMeshBuffer.TangentsVertexBuffer);
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			Data.TextureCoordinates[Channel] = Component(Elements[3 + Channel], StaticMeshBuffer.TexCoordVertexBuffer);
		Data.ColorComponent = Component(Elements[7], VertexBuffers.ColorVertexBuffer);
		Data.NumVertices = NumVertices;
		return true;
	}

	auto FLocalVertexFactory::GetPSOPrecacheVertexFetchElements(EVertexInputStreamType VertexInputStreamType,
		FVertexDeclarationElementList& Elements) -> void
	{
		Elements = {};
		if (VertexInputStreamType >= EVertexInputStreamType::Count) return;
		Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, sizeof(FVector3f));
		if (VertexInputStreamType == EVertexInputStreamType::PositionOnly) return;
		Elements[1] = FVertexElement(1, offsetof(FStaticMeshPackedTangentBasis, Normal), EVertexElementType::Short4N,
			1, sizeof(FStaticMeshPackedTangentBasis));
		if (VertexInputStreamType == EVertexInputStreamType::PositionAndNormalOnly) return;
		Elements[2] = FVertexElement(1, offsetof(FStaticMeshPackedTangentBasis, Tangent), EVertexElementType::Short4N,
			2, sizeof(FStaticMeshPackedTangentBasis));
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			Elements[3 + Channel] = FVertexElement(2, offsetof(FStaticMeshTexcoordVertex, TexCoords) + sizeof(FVector2f) * Channel,
				EVertexElementType::Float2, 3 + Channel, sizeof(FStaticMeshTexcoordVertex));
		Elements[7] = FVertexElement(3, offsetof(FStaticMeshColorVertex, Color), EVertexElementType::UByte4N,
			7, sizeof(FStaticMeshColorVertex));
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

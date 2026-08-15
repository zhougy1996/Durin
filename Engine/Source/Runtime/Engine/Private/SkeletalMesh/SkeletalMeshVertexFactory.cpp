#include "SkeletalMesh/SkeletalMeshVertexFactory.h"

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace Durin
{
	auto FSkeletalMeshVertexFactory::SetData(
		const FSkeletalMeshVertexBuffers& VertexBuffers) -> bool
	{
		check(!IsInitialized());
		const FStaticMeshVertexBuffers& Geometry = VertexBuffers.Geometry;
		const uint32 NumVertices = Geometry.PositionVertexBuffer.GetNumVertices();
		const auto& StaticMeshBuffer = Geometry.StaticMeshVertexBuffer;
		if (NumVertices == 0
			|| StaticMeshBuffer.GetNumVertices() != NumVertices
			|| Geometry.ColorVertexBuffer.GetNumVertices() != NumVertices
			|| VertexBuffers.InfluenceVertexBuffer.GetNumVertices() != NumVertices)
		{
			Data = {};
			return false;
		}

		const auto& Tangents = StaticMeshBuffer.TangentsVertexBuffer;
		const auto& TexCoords = StaticMeshBuffer.TexCoordVertexBuffer;
		const auto& Colors = Geometry.ColorVertexBuffer;
		const auto& Influences = VertexBuffers.InfluenceVertexBuffer;
		Data.PositionComponent = {
			.VertexBuffer = &Geometry.PositionVertexBuffer,
			.Offset = 0,
			.Stride = static_cast<uint16>(Geometry.PositionVertexBuffer.GetStride()),
			.Type = EVertexElementType::Float3,
			.AttributeIndex = 0,
			.StreamIndex = 0};
		Data.TangentBasisComponents[0] = {
			.VertexBuffer = &Tangents,
			.Offset = offsetof(FStaticMeshPackedTangentBasis, Normal),
			.Stride = static_cast<uint16>(Tangents.GetStride()),
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 1,
			.StreamIndex = 1};
		Data.TangentBasisComponents[1] = {
			.VertexBuffer = &Tangents,
			.Offset = offsetof(FStaticMeshPackedTangentBasis, Tangent),
			.Stride = static_cast<uint16>(Tangents.GetStride()),
			.Type = EVertexElementType::Short4N,
			.AttributeIndex = 2,
			.StreamIndex = 1};
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
		{
			Data.TextureCoordinates[Channel] = {
				.VertexBuffer = &TexCoords,
				.Offset = static_cast<uint32>(offsetof(FStaticMeshTexcoordVertex, TexCoords)
					+ sizeof(FVector2f) * Channel),
				.Stride = static_cast<uint16>(TexCoords.GetStride()),
				.Type = EVertexElementType::Float2,
				.AttributeIndex = static_cast<uint8>(3 + Channel),
				.StreamIndex = 2};
		}
		Data.ColorComponent = {
			.VertexBuffer = &Colors,
			.Offset = offsetof(FStaticMeshColorVertex, Color),
			.Stride = static_cast<uint16>(Colors.GetStride()),
			.Type = EVertexElementType::UByte4N,
			.AttributeIndex = 7,
			.StreamIndex = 3};
		Data.JointIndicesComponent = {
			.VertexBuffer = &Influences,
			.Offset = offsetof(FSkeletalMeshInfluenceVertex, JointIndices),
			.Stride = static_cast<uint16>(Influences.GetStride()),
			.Type = EVertexElementType::UShort4,
			.AttributeIndex = 8,
			.StreamIndex = 4};
		Data.JointWeightsComponent = {
			.VertexBuffer = &Influences,
			.Offset = offsetof(FSkeletalMeshInfluenceVertex, JointWeights),
			.Stride = static_cast<uint16>(Influences.GetStride()),
			.Type = EVertexElementType::Float4,
			.AttributeIndex = 9,
			.StreamIndex = 4};
		Data.NumVertices = NumVertices;
		return true;
	}

	auto FSkeletalMeshVertexFactory::GetDeclarationElements() const
		-> FVertexDeclarationElementList
	{
		FVertexDeclarationElementList Elements{};
		if (Data.NumVertices == 0 || !Data.PositionComponent.IsValid()
			|| !Data.ColorComponent.IsValid() || !Data.JointIndicesComponent.IsValid()
			|| !Data.JointWeightsComponent.IsValid()
			|| !std::ranges::all_of(Data.TangentBasisComponents,
				[](const FVertexStreamComponent& Component) { return Component.IsValid(); })
			|| !std::ranges::all_of(Data.TextureCoordinates,
				[](const FVertexStreamComponent& Component) { return Component.IsValid(); }))
		{
			return Elements;
		}
		Elements[0] = Data.PositionComponent.ToVertexElement();
		Elements[1] = Data.TangentBasisComponents[0].ToVertexElement();
		Elements[2] = Data.TangentBasisComponents[1].ToVertexElement();
		for (uint8 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			Elements[3 + Channel] = Data.TextureCoordinates[Channel].ToVertexElement();
		Elements[7] = Data.ColorComponent.ToVertexElement();
		Elements[8] = Data.JointIndicesComponent.ToVertexElement();
		Elements[9] = Data.JointWeightsComponent.ToVertexElement();
		return Elements;
	}

	auto FSkeletalMeshVertexFactory::IsDataValid() const -> bool
	{
		if (Data.NumVertices == 0 || !Data.PositionComponent.IsValid()
			|| !Data.ColorComponent.IsValid() || !Data.JointIndicesComponent.IsValid()
			|| !Data.JointWeightsComponent.IsValid()) return false;
		const FVertexBuffer* Position = Data.PositionComponent.VertexBuffer;
		const FVertexBuffer* Tangents = Data.TangentBasisComponents[0].VertexBuffer;
		const FVertexBuffer* TexCoords = Data.TextureCoordinates[0].VertexBuffer;
		const FVertexBuffer* Colors = Data.ColorComponent.VertexBuffer;
		const FVertexBuffer* Influences = Data.JointIndicesComponent.VertexBuffer;
		return Position->GetRHI() != nullptr && Tangents->GetRHI() != nullptr
			&& TexCoords->GetRHI() != nullptr && Colors->GetRHI() != nullptr
			&& Influences->GetRHI() != nullptr
			&& Data.TangentBasisComponents[1].VertexBuffer == Tangents
			&& Data.JointWeightsComponent.VertexBuffer == Influences
			&& std::ranges::all_of(Data.TextureCoordinates,
				[TexCoords](const FVertexStreamComponent& Component) {
					return Component.VertexBuffer == TexCoords;
				});
	}

	auto FSkeletalMeshVertexFactory::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		if (!IsDataValid()) return;
		SetDeclarationElements(GetDeclarationElements());
		SetStreams({
			{0, Data.PositionComponent.VertexBuffer->GetRHI(), 0, Data.PositionComponent.Stride},
			{1, Data.TangentBasisComponents[0].VertexBuffer->GetRHI(), 0,
				Data.TangentBasisComponents[0].Stride},
			{2, Data.TextureCoordinates[0].VertexBuffer->GetRHI(), 0,
				Data.TextureCoordinates[0].Stride},
			{3, Data.ColorComponent.VertexBuffer->GetRHI(), 0, Data.ColorComponent.Stride},
			{4, Data.JointIndicesComponent.VertexBuffer->GetRHI(), 0,
				Data.JointIndicesComponent.Stride}});
		FVertexFactory::InitRHI(RHICmdList);
	}
}

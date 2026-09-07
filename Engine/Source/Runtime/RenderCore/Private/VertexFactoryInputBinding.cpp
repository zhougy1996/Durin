#include "VertexFactory.h"
#include "GeometrySubmission.h"

namespace Durin
{
	auto FVertexFactoryInputBinding::ValidateInputs(const FGeometryDrawRange& Draw) const
		-> EGeometrySubmissionOutcome
	{
		if (!Declaration || Streams.empty()) return EGeometrySubmissionOutcome::ResourceFailure;
		for (size_t I = 0; I < Streams.size(); ++I)
		{
			const auto& Stream = Streams[I];
			if (!Stream.VertexBuffer) return EGeometrySubmissionOutcome::ResourceFailure;
			if (!Stream.Stride || !EnumHasAnyFlags(Stream.VertexBuffer->GetUsage(), EBufferUsageFlags::VertexBuffer))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			for (size_t J = 0; J < I; ++J)
				if (Streams[J].StreamIndex == Stream.StreamIndex) return EGeometrySubmissionOutcome::InvalidSubmission;
		}
		std::array<bool, 256> Attributes{};
		bool bHasAttributes = false;
		const auto& Actual = Declaration->GetElements();
		for (size_t I = 0; I < DeclarationElements.size(); ++I)
		{
			const auto& Element = DeclarationElements[I];
			if (Element.Type == EVertexElementType::None)
			{
				if (Actual[I].Type != EVertexElementType::None) return EGeometrySubmissionOutcome::InvalidSubmission;
				continue;
			}
			if (!(Element == Actual[I]) || Attributes[Element.AttributeIndex]) return EGeometrySubmissionOutcome::InvalidSubmission;
			Attributes[Element.AttributeIndex] = true;
			bHasAttributes = true;
			uint32 Bytes = 0;
			switch (Element.Type)
			{
			case EVertexElementType::Float1: case EVertexElementType::PackedNormal:
			case EVertexElementType::UByte4: case EVertexElementType::UByte4N:
			case EVertexElementType::Color: case EVertexElementType::Short2:
			case EVertexElementType::Short2N: case EVertexElementType::Half2:
			case EVertexElementType::UShort2: case EVertexElementType::UShort2N:
			case EVertexElementType::URGB10A2N: case EVertexElementType::UInt: Bytes = 4; break;
			case EVertexElementType::Float2: case EVertexElementType::Short4:
			case EVertexElementType::Half4: case EVertexElementType::Short4N:
			case EVertexElementType::UShort4: case EVertexElementType::UShort4N: Bytes = 8; break;
			case EVertexElementType::Float3: Bytes = 12; break;
			case EVertexElementType::Float4: Bytes = 16; break;
			default: return EGeometrySubmissionOutcome::Unsupported;
			}
			const auto Stream = std::ranges::find(Streams, Element.StreamIndex, &FVertexInputStream::StreamIndex);
			if (Stream == Streams.end() || Stream->Stride != Element.Stride
				|| static_cast<uint32>(Element.Offset) + Bytes > Element.Stride)
				return EGeometrySubmissionOutcome::InvalidSubmission;
			const FGeometryStreamRange Range{Stream->VertexBuffer->GetSize(),
				static_cast<uint64>(Stream->Offset) + Element.Offset, Element.Stride, Bytes};
			uint64 First = Draw.FirstElement, Count = Draw.ElementCount;
			if (Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance)
			{
				First = Draw.FirstInstance;
				Count = Draw.InstanceCount;
			}
			else if (Element.InputRate != FRHIVertexElementIdentity::EInputRate::Vertex)
				return EGeometrySubmissionOutcome::Unsupported;
			else if (Draw.bIndexed)
			{
				const int64 Min = static_cast<int64>(Draw.MinVertexIndex) + Draw.VertexOffset;
				if (Min < 0 || Draw.MinVertexIndex > Draw.MaxVertexIndex) return EGeometrySubmissionOutcome::InvalidSubmission;
				First = static_cast<uint64>(Min);
				Count = static_cast<uint64>(Draw.MaxVertexIndex) - Draw.MinVertexIndex + 1;
			}
			if (!Range.Contains(First, Count)) return EGeometrySubmissionOutcome::InvalidSubmission;
		}
		return bHasAttributes ? EGeometrySubmissionOutcome::Submitted : EGeometrySubmissionOutcome::InvalidSubmission;
	}
}

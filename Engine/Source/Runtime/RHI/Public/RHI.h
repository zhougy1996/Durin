#pragma once

#include "RHIFwd.h"

#include "PixelFormat.h"
#include "RHIGlobals.h"
#include "RHIDefinitions.h"
#include "DynamicRHI.h"
#include "RHIResources.h"
#include "RHICommandList.h"

namespace Durin
{
	// Maps one vertex-buffer field to the shader attribute that consumes it.
	struct FVertexElement
	{
		// The vertex stream index this element comes from.
		uint8 StreamIndex;
		// The offset in bytes of this element in the vertex stream.
		uint8 Offset;
		// The data type of this element.
		EVertexElementType Type = EVertexElementType::None;
		// The attribute index this element will be consumed as in shader. eg: for HLSL, this is the value specified in the semantic, for GLSL, this is the location.
		uint8 AttributeIndex;

		uint16 Stride;
		FRHIVertexElementIdentity::EInputRate InputRate =
			FRHIVertexElementIdentity::EInputRate::Vertex;

		FVertexElement() = default;
		FVertexElement(uint8 InStreamIndex, uint8 InOffset, EVertexElementType InType,
			uint8 InAttributeIndex, uint16 InStride,
			FRHIVertexElementIdentity::EInputRate InInputRate =
				FRHIVertexElementIdentity::EInputRate::Vertex)
			: StreamIndex(InStreamIndex)
			, Offset(InOffset)
			, Type(InType)
			, AttributeIndex(InAttributeIndex)
			, Stride(InStride)
			, InputRate(InInputRate)
		{
		}

		bool operator==(const FVertexElement& Other) const
		{
			return StreamIndex == Other.StreamIndex
				   && Offset == Other.Offset
				   && Type == Other.Type
				   && AttributeIndex == Other.AttributeIndex
				   && Stride == Other.Stride
				   && InputRate == Other.InputRate;
		}
	};
} // namespace Durin

#pragma once

#include "RHIShaderParameters.h"

namespace Durin
{
	struct FRHIShaderBufferBinding
	{
		FRHIBuffer* Buffer;
		FRHIBufferViewDesc Range;
	};

	// Dynamic offsets are supplied at bind time and are excluded from descriptor identity.
	inline auto ResolveShaderBufferBinding(const FRHIShaderParameterResource& Parameter) -> FRHIShaderBufferBinding
	{
		if (Parameter.Resource->GetResourceType() == ERHIResourceType::BufferView)
		{
			const auto* View = static_cast<const FRHIBufferView*>(Parameter.Resource);
			return {View->GetBuffer(), View->GetDesc()};
		}
		check(IsBufferResource(Parameter.Resource));
		auto* Buffer = static_cast<FRHIBuffer*>(Parameter.Resource);
		const uint64 Size = Parameter.Size ? Parameter.Size : Buffer->GetSize() - Parameter.Offset;
		auto Type = ERHIBufferViewType::Uniform;
		if (Parameter.Type == ERHIBindingType::StorageBuffer)
			Type = EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::ByteAddressBuffer)
				? ERHIBufferViewType::ByteAddressStorage : ERHIBufferViewType::StructuredStorage;
		return {Buffer, {Parameter.Type == ERHIBindingType::UniformBufferDynamic ? 0u : Parameter.Offset, Size, Type}};
	}
}

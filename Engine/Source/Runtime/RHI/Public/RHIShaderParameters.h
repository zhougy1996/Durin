#pragma once

#include "RHIResources.h"

namespace Durin
{
	class FRHIResource;

	struct FShaderParameterMetadata
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;
	};

	struct FShaderParameterBinding
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;
	};

	struct FRHIShaderParameterResource
	{
		FRHIResource* Resource = nullptr;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
	};
} // namespace Durin

#pragma once

#include "RHIResources.h"

namespace Durin
{
	class FRHIResource;

	enum class EShaderParameterMemberKind : uint8
	{
		Resource,
		Value,
		Struct
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

	struct FShaderParameterMemberMetadata
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 Size = 0;
		uint32 ArraySize = 1;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		EShaderParameterMemberKind Kind = EShaderParameterMemberKind::Resource;
	};

	struct FShaderParametersMetadata
	{
		const char* StructName = nullptr;
		uint32 StructSize = 0;
		uint32 StructAlignment = 0;
		std::span<const FShaderParameterMemberMetadata> Members;
	};

	struct FRHIShaderParameterResource
	{
		FRHIResource* Resource = nullptr;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 Offset = 0;
		uint32 Size = 0;
	};
} // namespace Durin

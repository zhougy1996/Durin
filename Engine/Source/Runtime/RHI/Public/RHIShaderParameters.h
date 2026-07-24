#pragma once

#include "RHIResources.h"

namespace Durin
{
	class FRHIResource;

	// Distinguishes resource bindings from inline values and nested parameter structures.
	enum class EShaderParameterMemberKind : uint8
	{
		Resource,
		Value,
		Struct
	};

	// Identifies one reflected shader resource at its byte offset and descriptor location.
	struct FShaderParameterBinding
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;
	};

	// Describes the layout and binding semantics of one C++ shader-parameter member.
	struct FShaderParameterMemberMetadata
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 Size = 0;
		uint32 ArraySize = 1;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		EShaderParameterMemberKind Kind = EShaderParameterMemberKind::Resource;
	};

	// Describes a complete shader-parameter structure without owning its member storage.
	struct FShaderParametersMetadata
	{
		const char* StructName = nullptr;
		uint32 StructSize = 0;
		uint32 StructAlignment = 0;
		const FShaderParametersMetadata* IncludedParameters = nullptr;
		std::span<const FShaderParameterMemberMetadata> Members;
	};

	// Owns member metadata while exposing a stable non-owning structure descriptor.
	struct FShaderParametersMetadataStorage
	{
		std::vector<FShaderParameterMemberMetadata> OwnedMembers;
		FShaderParametersMetadata Metadata;
	};

	// Carries one resolved RHI resource binding for command submission.
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

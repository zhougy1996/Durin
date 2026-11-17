#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "RHIShaderParameters.h"
#include "Shader/ShaderParameterMap.h"

namespace Durin
{
	// Detached semantic metadata. Numeric descriptor allocation belongs to reflection.
	struct FShaderParameterField
	{
		std::string QualifiedName;
		std::string ShaderName;
		EShaderParameterMemberKind Kind = EShaderParameterMemberKind::Resource;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;
		uint32 ValueSize = 0;
		bool bOptional = false;
		std::string ShaderType;
		std::string OwnerName;
		ERHIBindingType RuntimeType = ERHIBindingType::UniformBuffer;
		EShaderParameterBindingScope Scope = EShaderParameterBindingScope::Draw;
		uint32 ProviderIndex = 0;
		EShaderResourceShape ResourceShape = EShaderResourceShape::Unknown;
		uint32 ConstantBufferSize = 0;
		std::vector<FShaderConstantMember> ConstantMembers;
		auto operator==(const FShaderParameterField&) const -> bool = default;
	};

	// Owns canonical fields independently of registration and C++ structure lifetimes.
	struct FShaderParameterSchema
	{
		uint32 Version = 2;
		FXxHash128 Identity{};
		std::vector<FShaderParameterField> Fields;
	};

	// Fails on invalid layout, cycles, or ambiguous qualified/emitted names.
	RENDERCORE_API auto BuildShaderParameterSchema(const FShaderParametersMetadata& Metadata)
		-> std::expected<FShaderParameterSchema, std::string>;
	RENDERCORE_API auto ComposeShaderParameterSchemas(std::span<const FShaderParameterSchema> Schemas)
		-> std::expected<FShaderParameterSchema, std::string>;
	RENDERCORE_API auto CanonicalizeShaderParameterSchema(FShaderParameterSchema Schema)
		-> std::expected<FShaderParameterSchema, std::string>;
	RENDERCORE_API auto InferShaderParameterResourceShape(std::string_view ShaderType, ERHIBindingType Kind)
		-> EShaderResourceShape;

	// Exact compiled-product context; callers include their complete compile-key digest.
	struct FShaderParameterProduct
	{
		FShaderTargetIdentity Target;
		std::string ShaderType;
		std::string VertexFactoryType;
		uint32 PermutationId = 0;
		EShaderFrequency Frequency = EShaderFrequency::Vertex;
		FXxHash128 CompileKey{};
		auto operator==(const FShaderParameterProduct&) const -> bool = default;
	};

	// Value-owned candidate; publish only after validation against schema/product/reflection.
	struct FShaderParameterAllocationMap
	{
		uint32 Version = 2;
		FXxHash128 SchemaIdentity{};
		FXxHash128 Identity{};
		FShaderParameterProduct Product;
		FShaderParameterMap Parameters;
		auto operator==(const FShaderParameterAllocationMap&) const -> bool = default;
	};

	RENDERCORE_API auto BuildShaderParameterAllocationMap(const FShaderParameterSchema& Schema,
		const FShaderParameterProduct& Product, const FShaderReflectionData& Reflection)
		-> std::expected<FShaderParameterAllocationMap, std::string>;
	RENDERCORE_API auto ValidateShaderParameterAllocationMap(const FShaderParameterSchema& Schema,
		const FShaderParameterProduct& Product, const FShaderParameterAllocationMap& Map,
		const FShaderReflectionData& Reflection) -> std::expected<void, std::string>;
}

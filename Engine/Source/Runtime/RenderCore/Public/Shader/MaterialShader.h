#pragma once

#include "RenderResourceCreation.h"
#include "Shader/MaterialShaderIdentity.h"
#include "Shader/Shader.h"
#include "VertexFactory.h"

namespace Durin
{
	class FMaterialShaderMapPayload;

	class FMaterialShader : public FShader
	{
	public:
		using FShader::FShader;
	};

	class FMaterialShaderType : public FShaderType
	{
	public:
		RENDERCORE_API FMaterialShaderType(
			std::string_view InName,
			std::string_view InVirtualShaderPath,
			EShaderFrequency InFrequency,
			std::string_view InEntryPoint,
			FShaderFactoryFunction InFactory,
			FShouldCompilePermutationFunction InShouldCompilePermutation,
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
			const FShaderParametersMetadata* InParametersMetadata = nullptr);

		RENDERCORE_API static auto GetTypeList()
			-> const std::vector<const FMaterialShaderType*>&;
	};

	class FMeshMaterialShader : public FMaterialShader
	{
	public:
		using FMaterialShader::FMaterialShader;
	};

	class FMeshMaterialShaderType final : public FMaterialShaderType
	{
	public:
		RENDERCORE_API FMeshMaterialShaderType(
			std::string_view InName,
			std::string_view InVirtualShaderPath,
			EShaderFrequency InFrequency,
			std::string_view InEntryPoint,
			FShaderFactoryFunction InFactory,
			FShouldCompilePermutationFunction InShouldCompilePermutation,
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
			const FShaderParametersMetadata* InParametersMetadata = nullptr);

		RENDERCORE_API static auto GetTypeList()
			-> const std::vector<const FMeshMaterialShaderType*>&;
	};

	template<typename ShaderType>
	auto ShouldCompileMaterialShaderPermutation(
		const FShaderPermutationParameters& Parameters) -> bool
	{
		if constexpr (requires { ShaderType::ShouldCompilePermutation(Parameters); })
			return ShaderType::ShouldCompilePermutation(Parameters);
		return true;
	}

	template<typename ShaderType>
	auto ModifyMaterialShaderCompilationEnvironment(
		const FShaderPermutationParameters& Parameters,
		FShaderCompileOptions& Options) -> void
	{
		if constexpr (requires {
			ShaderType::ModifyCompilationEnvironment(Parameters, Options);
		})
			ShaderType::ModifyCompilationEnvironment(Parameters, Options);
	}

	template<typename ShaderType>
	auto MakeMaterialShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		const FShaderParametersMetadata* InParametersMetadata = nullptr)
		-> FMaterialShaderType
	{
		static_assert(std::derived_from<ShaderType, FMaterialShader>);
		return FMaterialShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint,
			&CreateDefaultShaderInstance<ShaderType>,
			&ShouldCompileMaterialShaderPermutation<ShaderType>,
			&ModifyMaterialShaderCompilationEnvironment<ShaderType>,
			InParametersMetadata);
	}

	#define DURIN_DECLARE_MATERIAL_SHADER(ShaderClass, SuperClass, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		ShaderClass(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection) \
			: SuperClass(InType, InShaderMap, InReflection) {} \
		static auto GetParametersMetadata() -> const FShaderParametersMetadata* \
		{ return GetShaderParametersMetadataOrNull<ShaderClass>(); } \
		static auto StaticType() -> FMaterialShaderType&; \
		static constexpr auto MaterialShaderVirtualPath() -> std::string_view { return VirtualPathLiteral; } \
		static constexpr auto MaterialShaderFrequency() -> EShaderFrequency { return FrequencyValue; } \
		static constexpr auto MaterialShaderEntryPoint() -> std::string_view { return EntryPointLiteral; }

	#define DURIN_IMPLEMENT_MATERIAL_SHADER(ShaderClass) \
		auto ShaderClass::StaticType() -> FMaterialShaderType& \
		{ \
			static FMaterialShaderType ShaderType = MakeMaterialShaderType<ShaderClass>( \
				#ShaderClass, ShaderClass::MaterialShaderVirtualPath(), \
				ShaderClass::MaterialShaderFrequency(), ShaderClass::MaterialShaderEntryPoint(), \
				ShaderClass::GetParametersMetadata()); \
			return ShaderType; \
		} \
		namespace { const FMaterialShaderType* const ShaderClass##_MaterialShaderRegistration = &ShaderClass::StaticType(); }

	template<typename ShaderType>
	auto MakeMeshMaterialShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		const FShaderParametersMetadata* InParametersMetadata = nullptr)
		-> FMeshMaterialShaderType
	{
		static_assert(std::derived_from<ShaderType, FMeshMaterialShader>);
		return FMeshMaterialShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint,
			&CreateDefaultShaderInstance<ShaderType>,
			&ShouldCompileMaterialShaderPermutation<ShaderType>,
			&ModifyMaterialShaderCompilationEnvironment<ShaderType>,
			InParametersMetadata);
	}

	#define DURIN_DECLARE_MESH_MATERIAL_SHADER(ShaderClass, SuperClass, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		ShaderClass(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection) \
			: SuperClass(InType, InShaderMap, InReflection) {} \
		static auto GetParametersMetadata() -> const FShaderParametersMetadata* \
		{ return GetShaderParametersMetadataOrNull<ShaderClass>(); } \
		static auto StaticType() -> FMeshMaterialShaderType&; \
		static constexpr auto MeshMaterialShaderVirtualPath() -> std::string_view { return VirtualPathLiteral; } \
		static constexpr auto MeshMaterialShaderFrequency() -> EShaderFrequency { return FrequencyValue; } \
		static constexpr auto MeshMaterialShaderEntryPoint() -> std::string_view { return EntryPointLiteral; }

	#define DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(ShaderClass) \
		auto ShaderClass::StaticType() -> FMeshMaterialShaderType& \
		{ \
			static FMeshMaterialShaderType ShaderType = MakeMeshMaterialShaderType<ShaderClass>( \
				#ShaderClass, ShaderClass::MeshMaterialShaderVirtualPath(), \
				ShaderClass::MeshMaterialShaderFrequency(), ShaderClass::MeshMaterialShaderEntryPoint(), \
				ShaderClass::GetParametersMetadata()); \
			return ShaderType; \
		} \
		namespace { const FMeshMaterialShaderType* const ShaderClass##_MeshMaterialShaderRegistration = &ShaderClass::StaticType(); }

	struct FMaterialShaderMapBuildInput
	{
		FMaterialShaderMapIdentity Identity;
		FRenderResourceGeneration Generation;
		std::string Target;
		uint32 LocalPermutationId = 0;
		const FVertexFactoryType* VertexFactoryType = nullptr;
		uint32 MeshPassKey = 0;
		uint32 MeshPermutationId = 0;
		std::span<const FShaderType* const> ShaderTypes;
		FShaderCompilerOutput CompilerOutput;
		FShaderCompileOptions CompileOptions;
		bool bContainsGeneratedMaterialStages = false;
		FMaterialProgramIdentity CompiledProgramIdentity;
		std::string CompiledTarget;
		bool bCreateRHIShaders = true;
	};

	struct FMaterialShaderMapCompileInput
	{
		FMaterialShaderMapIdentity Identity;
		FRenderResourceGeneration Generation;
		std::string Target;
		uint32 LocalPermutationId = 0;
		const FVertexFactoryType* VertexFactoryType = nullptr;
		uint32 MeshPassKey = 0;
		uint32 MeshPermutationId = 0;
		std::span<const FShaderType* const> ShaderTypes;
		FShaderCompileOptions CompileOptions;
		std::span<const FCompiledShader> GeneratedStages;
		FMaterialProgramIdentity CompiledProgramIdentity;
		std::string CompiledTarget;
		bool bCreateRHIShaders = true;
	};

	class FMaterialShaderMap
	{
	public:
		FMaterialShaderMap() = default;
		explicit operator bool() const { return Payload != nullptr; }

		RENDERCORE_API static auto TryCreate(
			FMaterialShaderMapBuildInput Input,
			FMaterialShaderMap& OutMap,
			std::string& OutError) -> bool;
		RENDERCORE_API static auto TryCompile(
			FMaterialShaderMapCompileInput Input,
			FMaterialShaderMap& OutMap,
			std::string& OutError) -> bool;

		RENDERCORE_API auto GetIdentity() const
			-> const FMaterialShaderMapIdentity&;
		RENDERCORE_API auto GetGeneration() const
			-> const FRenderResourceGeneration&;
		RENDERCORE_API auto GetCompatibilityHash() const -> FXxHash128;
		RENDERCORE_API auto GetCompatibilityText() const -> std::string_view;
		RENDERCORE_API auto GetPipelineLayout() const
			-> const FPipelineLayoutDesc&;

	private:
		explicit FMaterialShaderMap(
			std::shared_ptr<FMaterialShaderMapPayload> InPayload)
			: Payload(std::move(InPayload)) {}

		std::shared_ptr<FMaterialShaderMapPayload> Payload;
		template<typename ShaderType> friend class TMaterialShaderRef;
	};

	RENDERCORE_API auto GetShaderFromMaterialPayloadImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload,
		const FMaterialShaderType* Type) -> FShader*;
	RENDERCORE_API auto GetMaterialPayloadIdentityImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload)
		-> const FMaterialShaderMapIdentity&;
	RENDERCORE_API auto GetMaterialPayloadGenerationImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload)
		-> const FRenderResourceGeneration&;

	template<typename ShaderType>
	class TMaterialShaderRef
	{
	public:
		TMaterialShaderRef() = default;
		explicit TMaterialShaderRef(const FMaterialShaderMap& Map)
			: Payload(Map.Payload)
		{
			static_assert(std::derived_from<ShaderType, FMaterialShader>);
			if (Payload)
			{
				FShader* Base = GetMaterialShaderFromPayload(
					Payload, &ShaderType::StaticType());
				Shader = dynamic_cast<ShaderType*>(Base);
			}
		}

		auto GetShader() const -> ShaderType* { return Shader; }
		auto GetRHIShader(bool bRequired = true) const -> FRHIShader*
		{
			return Shader ? Shader->GetOrCreateRHIShader(bRequired) : nullptr;
		}
		auto GetIdentity() const -> const FMaterialShaderMapIdentity&
		{
			return GetMaterialPayloadIdentity(Payload);
		}
		auto GetGeneration() const -> const FRenderResourceGeneration&
		{
			return GetMaterialPayloadGeneration(Payload);
		}
		auto GetReflection() const -> const FShaderReflectionData&
		{
			check(Shader);
			return Shader->GetReflection();
		}
		explicit operator bool() const { return Shader != nullptr; }

	private:
		static auto GetMaterialShaderFromPayload(
			const std::shared_ptr<FMaterialShaderMapPayload>& InPayload,
			const FMaterialShaderType* Type) -> FShader*;
		static auto GetMaterialPayloadIdentity(
			const std::shared_ptr<FMaterialShaderMapPayload>& InPayload)
			-> const FMaterialShaderMapIdentity&;
		static auto GetMaterialPayloadGeneration(
			const std::shared_ptr<FMaterialShaderMapPayload>& InPayload)
			-> const FRenderResourceGeneration&;

		std::shared_ptr<FMaterialShaderMapPayload> Payload;
		ShaderType* Shader = nullptr;
	};

	template<typename ShaderType>
	auto TMaterialShaderRef<ShaderType>::GetMaterialShaderFromPayload(
		const std::shared_ptr<FMaterialShaderMapPayload>& InPayload,
		const FMaterialShaderType* Type) -> FShader*
	{
		return GetShaderFromMaterialPayloadImpl(InPayload, Type);
	}

	template<typename ShaderType>
	auto TMaterialShaderRef<ShaderType>::GetMaterialPayloadIdentity(
		const std::shared_ptr<FMaterialShaderMapPayload>& InPayload)
		-> const FMaterialShaderMapIdentity&
	{
		return GetMaterialPayloadIdentityImpl(InPayload);
	}

	template<typename ShaderType>
	auto TMaterialShaderRef<ShaderType>::GetMaterialPayloadGeneration(
		const std::shared_ptr<FMaterialShaderMapPayload>& InPayload)
		-> const FRenderResourceGeneration&
	{
		return GetMaterialPayloadGenerationImpl(InPayload);
	}

	template<typename ShaderType>
	auto SetShaderParameters(
		FRHICommandListBase& RHICmdList,
		const TMaterialShaderRef<ShaderType>& Shader,
		const typename ShaderType::FParameters& Parameters) -> void
	{
		SetShaderParameters(
			RHICmdList,
			TShaderRef<ShaderType>(Shader.GetShader(),
				Shader.GetShader() ? Shader.GetShader()->GetShaderMap() : nullptr),
			Parameters);
	}
}

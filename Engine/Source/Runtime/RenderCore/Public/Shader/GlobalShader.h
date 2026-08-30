#pragma once

#include "RenderResourceCreation.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	class FGlobalShaderMap;
	class FGlobalShaderMapPayload;
	class FGlobalShaderType;

	// Registers one exact Global Shader set for deterministic target inventory.
	class FGlobalShaderSetRegistration final
	{
	public:
		RENDERCORE_API FGlobalShaderSetRegistration(
			std::string_view Owner,
			std::string_view Name,
			EShaderRequestEligibility Eligibility,
			std::initializer_list<const FGlobalShaderType*> Types);
		[[nodiscard]] auto IsValid() const -> bool
		{
			return Registration.IsValid();
		}

	private:
		FShaderRequestRegistration Registration;
	};

	// Base class for fixed, non-Material shaders owned by the global shader map.
	class FGlobalShader : public FShader
	{
	public:
		using FShader::FShader;
	};

	class FGlobalShaderType final : public FShaderType
	{
	public:
		RENDERCORE_API FGlobalShaderType(
			std::string_view InName,
			std::string_view InVirtualShaderPath,
			EShaderFrequency InFrequency,
			std::string_view InEntryPoint,
			FShaderFactoryFunction InFactory,
			FShouldCompilePermutationFunction InShouldCompilePermutation,
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
			const FShaderParametersMetadata* InParametersMetadata = nullptr);

		RENDERCORE_API static auto GetTypeList()
			-> const std::vector<const FGlobalShaderType*>&;
	};

	template<typename ShaderType>
	auto ShouldCompileGlobalShaderPermutation(
		const FShaderPermutationParameters& Parameters) -> bool
	{
		if constexpr (requires { ShaderType::ShouldCompilePermutation(Parameters); })
			return ShaderType::ShouldCompilePermutation(Parameters);
		return true;
	}

	template<typename ShaderType>
	auto ModifyGlobalShaderCompilationEnvironment(
		const FShaderPermutationParameters& Parameters,
		FShaderCompileOptions& Options) -> void
	{
		if constexpr (requires {
			ShaderType::ModifyCompilationEnvironment(Parameters, Options);
		})
			ShaderType::ModifyCompilationEnvironment(Parameters, Options);
	}

	template<typename ShaderType>
	auto MakeGlobalShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		const FShaderParametersMetadata* InParametersMetadata = nullptr)
		-> FGlobalShaderType
	{
		static_assert(std::derived_from<ShaderType, FGlobalShader>);
		return FGlobalShaderType(
			InName,
			InVirtualShaderPath,
			InFrequency,
			InEntryPoint,
			&CreateDefaultShaderInstance<ShaderType>,
			&ShouldCompileGlobalShaderPermutation<ShaderType>,
			&ModifyGlobalShaderCompilationEnvironment<ShaderType>,
			InParametersMetadata);
	}

	#define DURIN_DECLARE_GLOBAL_SHADER(ShaderClass, SuperClass, VirtualPathLiteral, FrequencyValue, EntryPointLiteral) \
		ShaderClass(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection) \
			: SuperClass(InType, InShaderMap, InReflection) \
		{ \
		} \
 \
		static auto GetParametersMetadata() -> const FShaderParametersMetadata* \
		{ \
			return GetShaderParametersMetadataOrNull<ShaderClass>(); \
		} \
 \
		static auto StaticType() -> FGlobalShaderType&; \
		static constexpr auto GlobalShaderVirtualPath() -> std::string_view { return VirtualPathLiteral; } \
		static constexpr auto GlobalShaderFrequency() -> EShaderFrequency { return FrequencyValue; } \
		static constexpr auto GlobalShaderEntryPoint() -> std::string_view { return EntryPointLiteral; }

	#define DURIN_IMPLEMENT_GLOBAL_SHADER(ShaderClass) \
		auto ShaderClass::StaticType() -> FGlobalShaderType& \
		{ \
			static FGlobalShaderType ShaderType = MakeGlobalShaderType<ShaderClass>( \
				#ShaderClass, ShaderClass::GlobalShaderVirtualPath(), \
				ShaderClass::GlobalShaderFrequency(), ShaderClass::GlobalShaderEntryPoint(), \
				ShaderClass::GetParametersMetadata()); \
			return ShaderType; \
		} \
		namespace { const FGlobalShaderType* const ShaderClass##_GlobalShaderRegistration = &ShaderClass::StaticType(); }

	using FGlobalShaderDiagnosticReporter =
		std::function<void(const FRenderResourceCreateDiagnostic&)>;

	// Strong reference to one atomically published, generation-compatible set.
	class FGlobalShaderSetRef
	{
	public:
		FGlobalShaderSetRef() = default;

		explicit operator bool() const { return Payload != nullptr; }
		RENDERCORE_API auto GetGeneration() const
			-> const FRenderResourceGeneration&;
		RENDERCORE_API auto GetIdentity() const -> std::string_view;
		RENDERCORE_API auto GetPipelineLayout() const
			-> const FPipelineLayoutDesc&;

	private:
		explicit FGlobalShaderSetRef(
			std::shared_ptr<FGlobalShaderMapPayload> InPayload)
			: Payload(std::move(InPayload))
		{
		}

		std::shared_ptr<FGlobalShaderMapPayload> Payload;
		friend class FGlobalShaderMap;
		template<typename ShaderType> friend class TShaderMapRef;
	};

	template<typename ShaderType>
	class TShaderMapRef
	{
	public:
		TShaderMapRef() = default;
		explicit TShaderMapRef(const FGlobalShaderSetRef& Set)
			: Payload(Set.Payload)
		{
			static_assert(std::derived_from<ShaderType, FGlobalShader>);
			if (Payload)
			{
				FShader* Shader = GetShaderFromGlobalPayload(
					Payload, &ShaderType::StaticType());
				ShaderRef = TShaderRef<ShaderType>(
					static_cast<ShaderType*>(Shader), GetShaderMapFromGlobalPayload(Payload));
			}
		}

		auto GetShader() const -> ShaderType* { return ShaderRef.GetShader(); }
		auto GetRHIShader(bool bRequired = true) const -> FRHIShader*
		{
			return ShaderRef.GetRHIShader(bRequired);
		}
		auto GetShaderRef() const -> const TShaderRef<ShaderType>&
		{
			return ShaderRef;
		}
		auto GetGeneration() const -> const FRenderResourceGeneration&
		{
			check(Payload);
			return GetGlobalPayloadGeneration(Payload);
		}
		explicit operator bool() const { return static_cast<bool>(ShaderRef); }

	private:
		static auto GetShaderFromGlobalPayload(
			const std::shared_ptr<FGlobalShaderMapPayload>& InPayload,
			const FGlobalShaderType* InShaderType) -> FShader*;
		static auto GetShaderMapFromGlobalPayload(
			const std::shared_ptr<FGlobalShaderMapPayload>& InPayload)
			-> FShaderMapBase*;
		static auto GetGlobalPayloadGeneration(
			const std::shared_ptr<FGlobalShaderMapPayload>& InPayload)
			-> const FRenderResourceGeneration&;

		std::shared_ptr<FGlobalShaderMapPayload> Payload;
		TShaderRef<ShaderType> ShaderRef;
	};

	class FGlobalShaderMap
	{
	public:
		RENDERCORE_API auto ResolveShaderSet(
			std::string_view SectionIdentity,
			std::span<const FGlobalShaderType* const> ShaderTypes,
			bool bCreateRHIShaders = true,
			FGlobalShaderDiagnosticReporter ReportDiagnostic = {})
			-> FGlobalShaderSetRef;
		RENDERCORE_API auto SetGeneration_RenderThread(
			const FRenderResourceGeneration& InGeneration,
			bool bForceShaderRecompile) -> void;
		RENDERCORE_API auto ReleaseDeviceResources_RenderThread() -> void;
		RENDERCORE_API auto Shutdown_RenderThread() -> void;
		RENDERCORE_API auto GetGeneration_RenderThread() const
			-> const FRenderResourceGeneration&;
		RENDERCORE_API auto GetSectionCount() const -> size_t;

	private:
		struct FSectionEntry;
		std::unordered_map<std::string, std::unique_ptr<FSectionEntry>> Sections;
		FRenderResourceGeneration Generation;
		std::optional<uint64> ForceRecompileShaderGeneration;
	};

	RENDERCORE_API auto GetGlobalShaderMap() -> FGlobalShaderMap&;

	// Template bridge definitions are emitted for every shader type by callers;
	// these non-template helpers keep the payload representation private.
	RENDERCORE_API auto GetShaderFromGlobalPayloadImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload,
		const FGlobalShaderType* ShaderType) -> FShader*;
	RENDERCORE_API auto GetShaderMapFromGlobalPayloadImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload)
		-> FShaderMapBase*;
	RENDERCORE_API auto GetGlobalPayloadGenerationImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload)
		-> const FRenderResourceGeneration&;

	template<typename ShaderType>
	auto TShaderMapRef<ShaderType>::GetShaderFromGlobalPayload(
		const std::shared_ptr<FGlobalShaderMapPayload>& InPayload,
		const FGlobalShaderType* InShaderType) -> FShader*
	{
		return GetShaderFromGlobalPayloadImpl(InPayload, InShaderType);
	}

	template<typename ShaderType>
	auto TShaderMapRef<ShaderType>::GetShaderMapFromGlobalPayload(
		const std::shared_ptr<FGlobalShaderMapPayload>& InPayload)
		-> FShaderMapBase*
	{
		return GetShaderMapFromGlobalPayloadImpl(InPayload);
	}

	template<typename ShaderType>
	auto TShaderMapRef<ShaderType>::GetGlobalPayloadGeneration(
		const std::shared_ptr<FGlobalShaderMapPayload>& InPayload)
		-> const FRenderResourceGeneration&
	{
		return GetGlobalPayloadGenerationImpl(InPayload);
	}

	template<typename ShaderType>
	auto SetShaderParameters(
		FRHICommandListBase& RHICmdList,
		const TShaderMapRef<ShaderType>& Shader,
		const typename ShaderType::FParameters& Parameters) -> void
	{
		SetShaderParameters(RHICmdList, Shader.GetShaderRef(), Parameters);
	}

	template<typename ShaderType>
	auto SetShaderParameters(
		FRHICommandListBase& RHICmdList,
		const TShaderMapRef<ShaderType>& Shader,
		const FRDGShaderParameterScope& GraphParameters,
		const typename ShaderType::FParameters& OrdinaryParameters) -> void
	{
		SetShaderParameters(
			RHICmdList, Shader.GetShaderRef(), GraphParameters,
			OrdinaryParameters);
	}
} // namespace Durin

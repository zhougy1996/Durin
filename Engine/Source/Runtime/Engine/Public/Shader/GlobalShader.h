#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

#include "Shader/Shader.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	class FGlobalShaderMap;
	// Startup joins the required jobs. Render-thread reloads leave the current map available.
	ENGINE_API void CompileGlobalShaderMap(bool bForceRecompile = false);
	class FGlobalShaderType;

	// Registers one exact Global Shader set for deterministic target inventory.
	class FGlobalShaderSetRegistration final
	{
	public:
		ENGINE_API FGlobalShaderSetRegistration(
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
		ENGINE_API FGlobalShaderType(
			std::string_view InName,
			std::string_view InVirtualShaderPath,
			EShaderFrequency InFrequency,
			std::string_view InEntryPoint,
			FShaderFactoryFunction InFactory,
			FShouldCompilePermutationFunction InShouldCompilePermutation,
			FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
			const FShaderParametersMetadata* InParametersMetadata = nullptr);

		ENGINE_API static auto GetTypeList()
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

	template<typename ShaderType>
	class TShaderMapRef : public TShaderRef<ShaderType>
	{
	public:
		TShaderMapRef() = default;
		explicit TShaderMapRef(const FGlobalShaderMap& Map);
		auto GetPipelineLayout() const -> const FPipelineLayoutDesc& { return Section->GetMergedPipelineLayout(); }

	private:
		explicit TShaderMapRef(std::shared_ptr<FShaderMapBase> InSection)
			: TShaderRef<ShaderType>(InSection ? static_cast<ShaderType*>(
				InSection->GetShader(&ShaderType::StaticType())) : nullptr, InSection.get())
			, Section(std::move(InSection)) {}
		std::shared_ptr<FShaderMapBase> Section;
	};

	class FGlobalShaderMap
	{
	public:
		template<typename ShaderType>
		auto GetShader() const -> TShaderMapRef<ShaderType> { return TShaderMapRef<ShaderType>(*this); }
		auto HasShader(const FGlobalShaderType& Type) const -> bool { return PublishedShaders.contains(&Type); }
		ENGINE_API auto ProcessCompilationResults_RenderThread() -> bool;
		ENGINE_API auto ReleaseDeviceResources_RenderThread() -> void;
		ENGINE_API auto Shutdown_RenderThread() -> void;

	private:
		friend ENGINE_API void CompileGlobalShaderMap(bool bForceRecompile);
		template<typename ShaderType> friend class TShaderMapRef;
		ENGINE_API auto FindSection(const FGlobalShaderType& Type) const -> std::shared_ptr<FShaderMapBase>;
		struct FCompilation;
		std::shared_ptr<FCompilation> PendingCompilation;
		std::unordered_map<const FShaderType*, std::shared_ptr<FShaderMapBase>> PublishedShaders;
	};

	template<typename ShaderType>
	TShaderMapRef<ShaderType>::TShaderMapRef(const FGlobalShaderMap& Map)
		: TShaderMapRef(Map.FindSection(ShaderType::StaticType())) {}

	ENGINE_API auto GetGlobalShaderMap() -> FGlobalShaderMap&;

} // namespace Durin

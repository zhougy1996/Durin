#include "Shader/Shader.h"

namespace Durin
{
	namespace
	{
		class FShaderTypeRegistry
		{
		public:
			auto Register(const FShaderType* ShaderType) -> void
			{
				checkf(ShaderType, "Shader type must be valid");
				const FName TypeName = ShaderType->GetFName();
				const auto FoundIt = std::ranges::find_if(ShaderTypes, [TypeName](const FShaderType* ExistingType) {
					return ExistingType && ExistingType->GetFName() == TypeName;
				});
				checkf(FoundIt == ShaderTypes.end(), "Duplicate shader type registration: {}", TypeName.ToString());
				ShaderTypes.push_back(ShaderType);
			}

			auto Unregister(const FShaderType* ShaderType) -> void
			{
				const auto FoundIt = std::ranges::find(ShaderTypes, ShaderType);
				if (FoundIt != ShaderTypes.end())
				{
					ShaderTypes.erase(FoundIt);
				}
			}

			auto GetTypeList() const -> const std::vector<const FShaderType*>&
			{
				return ShaderTypes;
			}

		private:
			std::vector<const FShaderType*> ShaderTypes;
		};

		auto GetShaderTypeRegistry() -> FShaderTypeRegistry&
		{
			static FShaderTypeRegistry Registry;
			return Registry;
		}

		auto MakeDefaultShaderInstance(
			const FShaderType* ShaderType,
			FShaderMapBase* ShaderMap,
			const FShaderReflectionData& Reflection
		) -> std::unique_ptr<FShader>
		{
			return std::make_unique<FShader>(ShaderType, ShaderMap, Reflection);
		}

	} // namespace

	FShaderType::FShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		std::string_view InDebugName,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
		const FShaderParametersMetadata* InParametersMetadata
	)
		: Name(InName)
		, TypeName(Name)
		, VirtualShaderPath(InVirtualShaderPath)
		, Frequency(InFrequency)
		, EntryPoint(InEntryPoint)
		, DebugName(InDebugName.empty() ? InName : InDebugName)
		, ParametersMetadata(InParametersMetadata)
		, Factory(InFactory ? InFactory : &MakeDefaultShaderInstance)
		, ShouldCompilePermutationFn(InShouldCompilePermutation)
		, ModifyCompilationEnvironmentFn(InModifyCompilationEnvironment)
	{
		GetShaderTypeRegistry().Register(this);
	}

	FShaderType::~FShaderType()
	{
		GetShaderTypeRegistry().Unregister(this);
	}

	auto FShaderType::CreateShaderInstance(FShaderMapBase* ShaderMap, const FShaderReflectionData& Reflection) const -> std::unique_ptr<FShader>
	{
		return Factory(this, ShaderMap, Reflection);
	}

	auto FShaderType::ShouldCompilePermutation(const FShaderPermutationParameters& Parameters) const -> bool
	{
		return ShouldCompilePermutationFn ? ShouldCompilePermutationFn(Parameters) : true;
	}

	auto FShaderType::ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions) const -> void
	{
		if (ModifyCompilationEnvironmentFn)
		{
			ModifyCompilationEnvironmentFn(Parameters, CompileOptions);
		}
	}

	auto FShaderType::GetTypeList() -> const std::vector<const FShaderType*>&
	{
		return GetShaderTypeRegistry().GetTypeList();
	}

	FShader::FShader(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection)
		: Type(InType)
		, ShaderMap(InShaderMap)
		, Reflection(InReflection)
	{
	}

	auto FShader::GetOrCreateRHIShader(bool bRequired) -> FRHIShader*
	{
		return (ShaderMap && Type) ? ShaderMap->GetOrCreateShaderRHI(Type, bRequired) : nullptr;
	}

	auto FShader::InitializeParameterBindings() -> FShaderOperationResult
	{
		ParameterBindings.clear();
		return BuildShaderParameterBindings(Type ? Type->GetParametersMetadata() : nullptr, Reflection, ParameterBindings);
	}

}

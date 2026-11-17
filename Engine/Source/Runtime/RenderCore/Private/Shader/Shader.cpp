#include "Shader/Shader.h"
#include "Shader/ShaderParameterSchema.h"
#include "ShaderParameterSchemaInternal.h"

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
		if (ParametersMetadata)
		{
			auto Schema = Private::BuildRegisteredShaderParameterSchema(*ParametersMetadata, ParameterHostBindings);
			if (Schema) ParameterSchema = std::make_unique<const FShaderParameterSchema>(std::move(*Schema));
			else ParameterSchemaError = Schema.error();
		}
		GetShaderTypeRegistry().Register(this);
	}

	auto FShaderType::GetParameterSchemaIdentity() const -> std::expected<FXxHash128, std::string>
	{
		if (!ParameterSchemaError.empty()) return std::unexpected(ParameterSchemaError);
		return ParameterSchema ? ParameterSchema->Identity : FXxHash128{};
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

	auto FShaderType::ApplyParameterCompilationEnvironment(FShaderCompileOptions& CompileOptions) const -> void
	{
		const auto SetMacro = [&](std::string MacroName, std::string Value) {
			const auto Existing = std::ranges::find(CompileOptions.Macros, MacroName, &FShaderMacroDefinition::Name);
			if (Existing == CompileOptions.Macros.end()) CompileOptions.Macros.emplace_back(std::move(MacroName), std::move(Value));
			else Existing->Value = std::move(Value);
		};
		if (Frequency == EShaderFrequency::Vertex || Frequency == EShaderFrequency::Fragment)
			SetMacro("DURIN_STAGE_PARAMETER_SPACES", "1");
		if (ParametersMetadata)
		{
			// Invalid metadata is rejected at binding admission; its distinct key must
			// never select a previously accepted valid schema.
			const auto MacroName = std::format("DURIN_PARAMETER_SCHEMA_{}", FXxHash128::HashBuffer(Name).ToString());
			SetMacro(MacroName, ParameterSchema ? std::format("\"{}\"", ParameterSchema->Identity.ToString()) : "invalid");
		}
	}

	auto FShaderType::ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions) const -> void
	{
		if (ModifyCompilationEnvironmentFn)
		{
			ModifyCompilationEnvironmentFn(Parameters, CompileOptions);
		}
		ApplyParameterCompilationEnvironment(CompileOptions);
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
		Bindings = {};
		if (Type) return Bindings.Bind(*Type, Reflection);
		auto Map = FShaderParameterMap::FromReflection(Reflection);
		if (!Map) return std::unexpected(Map.error());
		return Bindings.Bind(Type ? Type->GetParametersMetadata() : nullptr, *Map);
	}

}

#include "Shader/GlobalShader.h"

#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		class FGlobalShaderTypeRegistry
		{
		public:
			auto Register(const FGlobalShaderType* Type) -> void
			{
				check(Type);
				const auto It = std::ranges::find_if(Types, [Type](const auto* Existing) {
					return Existing->GetFName() == Type->GetFName();
				});
				checkf(It == Types.end(), "Duplicate global shader implementation: {}", Type->GetName());
				Types.push_back(Type);
			}

			auto GetTypes() const -> const std::vector<const FGlobalShaderType*>&
			{
				return Types;
			}

		private:
			std::vector<const FGlobalShaderType*> Types;
		};

		auto GetGlobalShaderTypeRegistry() -> FGlobalShaderTypeRegistry&
		{
			static FGlobalShaderTypeRegistry Registry;
			return Registry;
		}

		auto MakeSetIdentity(
			std::string_view SectionIdentity,
			std::span<const FGlobalShaderType* const> Types) -> std::string
		{
			std::string Result(SectionIdentity);
			for (const FGlobalShaderType* Type : Types)
			{
				Result.append("|");
				Result.append(Type ? Type->GetName() : "<null>");
			}
			return Result;
		}
	} // namespace

	class FGlobalShaderMapPayload
	{
	public:
		std::shared_ptr<FShaderMapBase> ShaderMap;
		FRenderResourceGeneration Generation;
		std::string Identity;
	};

	struct FGlobalShaderMap::FSectionEntry
	{
		std::string TypeIdentity;
		std::vector<const FGlobalShaderType*> Types;
		TRenderResourceCreationSlot<std::shared_ptr<FGlobalShaderMapPayload>> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FGlobalShaderType::FGlobalShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
		const FShaderParametersMetadata* InParametersMetadata)
		: FShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint, {}, InFactory,
			InShouldCompilePermutation, InModifyCompilationEnvironment,
			InParametersMetadata)
	{
		GetGlobalShaderTypeRegistry().Register(this);
	}

	auto FGlobalShaderType::GetTypeList()
		-> const std::vector<const FGlobalShaderType*>&
	{
		return GetGlobalShaderTypeRegistry().GetTypes();
	}

	auto FGlobalShaderSetRef::GetGeneration() const
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}

	auto FGlobalShaderSetRef::GetIdentity() const -> std::string_view
	{
		return Payload ? std::string_view(Payload->Identity) : std::string_view{};
	}

	auto FGlobalShaderSetRef::GetPipelineLayout() const
		-> const FPipelineLayoutDesc&
	{
		check(Payload && Payload->ShaderMap);
		return Payload->ShaderMap->GetMergedPipelineLayout();
	}

	auto FGlobalShaderMap::ResolveShaderSet(
		std::string_view SectionIdentity,
		std::span<const FGlobalShaderType* const> ShaderTypes,
		bool bCreateRHIShaders,
		FGlobalShaderDiagnosticReporter ReportDiagnostic)
		-> FGlobalShaderSetRef
	{
		checkf(!SectionIdentity.empty(), "Global shader section identity must not be empty");
		checkf(!ShaderTypes.empty(), "Global shader set must contain at least one type");

		std::vector<const FGlobalShaderType*> StableTypes(ShaderTypes.begin(), ShaderTypes.end());
		std::ranges::sort(StableTypes, {}, [](const FGlobalShaderType* Type) {
			return Type ? Type->GetName() : std::string_view{};
		});
		const std::string TypeIdentity = MakeSetIdentity(SectionIdentity, StableTypes);
		auto [It, bInserted] = Sections.try_emplace(std::string(SectionIdentity));
		if (bInserted)
		{
			It->second = std::make_unique<FSectionEntry>();
			It->second->TypeIdentity = TypeIdentity;
			It->second->Types = StableTypes;
		}
		FSectionEntry& Entry = *It->second;
		checkf(Entry.TypeIdentity == TypeIdentity,
			"Global shader section '{}' was resolved with a conflicting type set",
			SectionIdentity);

		using FResult = TRenderResourceCreateResult<
			std::shared_ptr<FGlobalShaderMapPayload>>;
		auto* Published = Entry.Slot.Resolve(
			Generation,
			[&]() -> FResult {
				for (const FGlobalShaderType* Type : Entry.Types)
				{
					if (Type == nullptr)
					{
						return FResult::Failure({
							.Category = ERenderResourceCreateErrorCategory::InvalidConfiguration,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Message = "Global shader set contains a null type.",
							.RetryDependencies = ERenderResourceGenerationDependency::Manual});
					}
				}
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					ForceRecompileShaderGeneration == Generation.Shader;
				auto Candidate = std::make_shared<FGlobalShaderMapPayload>();
				Candidate->ShaderMap = std::make_shared<FShaderMapBase>();
				Candidate->Generation = Generation;
				Candidate->Identity = TypeIdentity;
				std::vector<const FShaderType*> BaseTypes(
					Entry.Types.begin(), Entry.Types.end());
				std::string Error;
				if (!Candidate->ShaderMap->InitializeFromShaderTypes(
						BaseTypes, Options, Error))
				{
					return FResult::Failure({
						.Category = ERenderResourceCreateErrorCategory::ShaderCompile,
						.Context = std::string(SectionIdentity),
						.Identity = TypeIdentity,
						.Message = std::move(Error),
						.RetryDependencies = ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual});
				}
				for (const FGlobalShaderType* Type : Entry.Types)
				{
					FShader* Shader = Candidate->ShaderMap->GetShader(Type);
					if (Shader == nullptr)
					{
						return FResult::Failure({
							.Category = ERenderResourceCreateErrorCategory::ShaderBinding,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Message = std::format("Global shader set is missing type '{}'.", Type->GetName()),
							.RetryDependencies = ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual});
					}
					if (bCreateRHIShaders
						&& Shader->GetOrCreateRHIShader(false) == nullptr)
					{
						return FResult::Failure({
							.Category = ERenderResourceCreateErrorCategory::RHIResource,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Message = std::format(
								"RHI shader creation returned null for type='{}', path='{}', entry='{}', frequency={}.",
								Type->GetName(), Type->GetVirtualShaderPath(),
								Type->GetEntryPoint(),
								static_cast<uint8>(Type->GetFrequency())),
							.RetryDependencies = ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual});
					}
				}
				return FResult::Success(std::move(Candidate));
			},
			[&](const FRenderResourceCreateDiagnostic& Diagnostic) {
				if (ReportDiagnostic) ReportDiagnostic(Diagnostic);
			});
		return Published ? FGlobalShaderSetRef(*Published) : FGlobalShaderSetRef{};
	}

	auto FGlobalShaderMap::SetGeneration_RenderThread(
		const FRenderResourceGeneration& InGeneration,
		bool bForceShaderRecompile) -> void
	{
		Generation = InGeneration;
		ForceRecompileShaderGeneration = bForceShaderRecompile
			? std::optional<uint64>(Generation.Shader) : std::nullopt;
	}

	auto FGlobalShaderMap::ReleaseDeviceResources_RenderThread() -> void
	{
		for (auto& [Name, Entry] : Sections)
		{
			Entry->Slot.Reset();
		}
		ClearShaderMapResourceCache();
	}

	auto FGlobalShaderMap::Shutdown_RenderThread() -> void
	{
		Sections.clear();
		Generation = {};
		ForceRecompileShaderGeneration.reset();
	}

	auto FGlobalShaderMap::GetGeneration_RenderThread() const
		-> const FRenderResourceGeneration&
	{
		return Generation;
	}

	auto FGlobalShaderMap::GetSectionCount() const -> size_t
	{
		return Sections.size();
	}

	auto GetGlobalShaderMap() -> FGlobalShaderMap&
	{
		static FGlobalShaderMap Map;
		return Map;
	}

	auto GetShaderFromGlobalPayloadImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload,
		const FGlobalShaderType* ShaderType) -> FShader*
	{
		return Payload && Payload->ShaderMap
			? Payload->ShaderMap->GetShader(ShaderType) : nullptr;
	}

	auto GetShaderMapFromGlobalPayloadImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload)
		-> FShaderMapBase*
	{
		return Payload && Payload->ShaderMap ? Payload->ShaderMap.get() : nullptr;
	}

	auto GetGlobalPayloadGenerationImpl(
		const std::shared_ptr<FGlobalShaderMapPayload>& Payload)
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}
} // namespace Durin

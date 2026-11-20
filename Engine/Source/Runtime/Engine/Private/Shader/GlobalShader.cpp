#include "Shader/GlobalShader.h"
#include "RenderResourcePreparation.h"
#include "Shader/ShaderMapPreparation.h"
#include "Shader/ShaderData.h"
#include "Shader/ShaderCompilingManager.h"

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

	FGlobalShaderSetRegistration::FGlobalShaderSetRegistration(
		std::string_view Owner,
		std::string_view Name,
		EShaderRequestEligibility Eligibility,
		std::initializer_list<const FGlobalShaderType*> Types)
	{
		FShaderRuntimeRequest Request;
		Request.Category = EShaderRuntimeRequestCategory::GlobalSet;
		Request.Owner = Owner;
		Request.Name = Name;
		std::vector<const FShaderType*> BaseTypes(Types.begin(), Types.end());
		FShaderOperationResult Error;
		Error = RegisterShaderRuntimeRequest(
			std::move(Request), Eligibility, Registration, BaseTypes);
		requiref(Registration.IsValid(),
			"Global Shader set registration failed for '{}': {}", Name, std::string(Error.error()));
	}

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

	struct FGlobalShaderMap::FCompilation
	{
		struct FSection
		{
			FRegisteredGlobalShaderSet Set;
			FShaderCompileOptions Options;
			std::shared_ptr<FShaderCompilerOutput> Output = std::make_shared<FShaderCompilerOutput>();
			FShaderBuildHandle Job;
		};
		std::vector<FSection> Sections;
	};

	void CompileGlobalShaderMap(bool bForceRecompile)
	{
		const bool bCooked = GetShaderDataDomain() == EShaderDataDomain::Cooked;
		auto Batch = std::make_shared<FGlobalShaderMap::FCompilation>();
		for (auto& Set : GetRegisteredGlobalShaderSets(bCooked ? EShaderTargetProfile::Game : EShaderTargetProfile::EditorValidation))
		{
			auto& Section = Batch->Sections.emplace_back();
			Section.Set = std::move(Set);
			const auto& Types = Section.Set.Types;
			auto& Options = Section.Options;
			FShaderOperationResult Result;
			if (Types.empty()) Result = std::unexpected(std::string("No registered build types."));
			else if (bCooked) Result = LoadCookedShaderRuntimeRequest(Section.Set.Name, Types, *Section.Output);
			else if ((Result = BuildShaderMapCompileOptions(Types, nullptr, Options)))
			{
				Options.bForceRecompile = bForceRecompile;
				for (const auto* Type : Types)
					Type->ModifyCompilationEnvironment({Type, Type->GetVirtualShaderPath(), Type->GetEntryPoint(),
						Type->GetFrequency(), Options.Target}, Options);
				auto& Manager = FShaderCompilingManager::Get();
				Section.Job = Manager.PrepareJobs(Options.VirtualShaderPath, Options, {.Priority = EShaderBuildPriority::High},
					[Dest = Section.Output](FShaderCompilerOutput Output) { *Dest = std::move(Output); });
				Manager.SubmitJobs(Section.Job);
			}
			if (!Result) *Section.Output = FShaderCompilerOutput::Failure(std::move(Result.error()));
		}
		auto Publish = [Batch](FRHICommandListImmediate&) {
			auto& Map = GetGlobalShaderMap();
			Map.PendingCompilation = Batch;
			Map.ProcessCompilationResults_RenderThread();
		};
		if (!IsInRenderingThread())
			for (const auto& Section : Batch->Sections)
				if (!Section.Job.IsComplete()) require(Section.Job.Wait() == EShaderBuildWaitResult::Completed);
		EnqueueRenderCommand("PublishGlobalShaders", std::move(Publish));
	}

	auto FGlobalShaderMap::ProcessCompilationResults_RenderThread() -> void
	{
		if (!PendingCompilation) return;
		for (const auto& Section : PendingCompilation->Sections)
			if (!Section.Job.IsComplete()) return;
		auto Batch = std::move(PendingCompilation);
		decltype(PublishedSections) Candidate;
		decltype(PublishedShaders) Shaders;
		for (const auto& Section : Batch->Sections)
		{
			const auto& Output = *Section.Output;
			if (!Output)
			{
				if (Output.Status == EShaderBuildStatus::Error)
					DURIN_ERROR("Global shader set '{}' failed: {}", Section.Set.Name, FormatShaderCompilerErrors(Output.Errors));
				return;
			}
			auto Payload = std::make_shared<FGlobalShaderMapPayload>();
			Payload->ShaderMap = std::make_shared<FShaderMapBase>();
			if (auto Result = Payload->ShaderMap->Initialize(Section.Set.Types, Output, Section.Options); !Result)
			{
				DURIN_ERROR("Global shader set '{}' failed: {}", Section.Set.Name, Result.error());
				return;
			}
			Payload->Generation = Generation;
			Payload->Identity = Section.Set.Name;
			for (const auto* Type : Section.Set.Types)
			{
				Payload->Identity.append("|").append(Type->GetName());
				Shaders.try_emplace(Type, FGlobalShaderSetRef(Payload));
			}
			if (!Candidate.emplace(Section.Set.Name, std::move(Payload)).second)
			{
				DURIN_ERROR("Global shader set '{}' is registered more than once.", Section.Set.Name);
				return;
			}
		}
		PublishedSections.swap(Candidate);
		PublishedShaders.swap(Shaders);
	}

	auto FGlobalShaderMap::FindShaderSection(const FGlobalShaderType& Type) const -> FGlobalShaderSetRef
	{
		const auto Found = PublishedShaders.find(&Type);
		return Found != PublishedShaders.end() ? Found->second : FGlobalShaderSetRef{};
	}

	auto FGlobalShaderMap::ResolveShaderSet(
		std::string_view SectionIdentity,
		std::span<const FGlobalShaderType* const> ShaderTypes,
		FGlobalShaderSetRef& OutSet,
		bool bCreateRHIShaders,
		FGlobalShaderDiagnosticReporter ReportDiagnostic)
		-> FShaderMapQueryResult
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
							.Reason = ERenderResourceCreateErrorReason::ShaderFailure,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Cause = std::string("Material shader map contains a null shader type."),
							.RetryDependencies = ERenderResourceGenerationDependency::Manual});
					}
				}
				std::shared_ptr<FGlobalShaderMapPayload> Candidate;
				FShaderMapQueryResult ShaderResult;
				const auto Prepared = PublishedSections.find(std::string(SectionIdentity));
				if (Prepared != PublishedSections.end() && Prepared->second->Identity == TypeIdentity
					&& Prepared->second->Generation == Generation)
					Candidate = Prepared->second;
				else
				{
					Candidate = std::make_shared<FGlobalShaderMapPayload>();
					Candidate->ShaderMap = std::make_shared<FShaderMapBase>();
					Candidate->Generation = Generation;
					Candidate->Identity = TypeIdentity;
					std::vector<const FShaderType*> BaseTypes(
						Entry.Types.begin(), Entry.Types.end());
					if (GetShaderDataDomain() == EShaderDataDomain::Cooked)
					{
						FShaderCompilerOutput Output;
						ShaderResult = LoadCookedShaderRuntimeRequest(SectionIdentity, BaseTypes, Output);
						if (ShaderResult) ShaderResult = Candidate->ShaderMap->Initialize(BaseTypes, Output);
					}
					else
					{
						FShaderCompileOptions Options;
						Options.bForceRecompile =
							ForceRecompileShaderGeneration == Generation.Shader;
						ShaderResult = InitializeShaderMapFromTypes(*Candidate->ShaderMap, BaseTypes, Options);
					}
				}
				if (ShaderResult.Status == ERenderResourceAvailability::Pending) return FResult::Pending();
				if (ShaderResult.Status == ERenderResourceAvailability::Canceled) return FResult::Canceled();
				if (!ShaderResult)
				{
					return FResult::Failure({
						.Category = ERenderResourceCreateErrorCategory::ShaderCompile,
						.Reason = ERenderResourceCreateErrorReason::ShaderFailure,
						.Context = std::string(SectionIdentity),
						.Identity = TypeIdentity,
						.Cause = std::move(ShaderResult.Error),
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
							.Reason = ERenderResourceCreateErrorReason::ShaderFailure,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Cause = std::format("Material shader map is missing type '{}'.", (std::string(Type->GetName()))),
							.RetryDependencies = ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual});
					}

					if (bCreateRHIShaders
						&& Shader->GetOrCreateRHIShader(false) == nullptr)
					{

						return FResult::Failure({
							.Category = ERenderResourceCreateErrorCategory::RHIResource,
							.Reason = ERenderResourceCreateErrorReason::ShaderFailure,
							.Context = std::string(SectionIdentity),
							.Identity = TypeIdentity,
							.Cause = std::format("RHI shader creation returned null for Material type '{}'.", (std::string(Type->GetName()))),
							.RetryDependencies = ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual});
					}
				}
				return FResult::Success(std::move(Candidate));
			},
			[&](const FRenderResourceCreateDiagnostic& Diagnostic) {
				if (ReportDiagnostic) ReportDiagnostic(Diagnostic);
			});
		OutSet = Published ? FGlobalShaderSetRef(*Published) : FGlobalShaderSetRef{};
		const auto Availability = Entry.Slot.GetAvailability();
		if (Availability == ERenderResourceAvailability::Failed)
			return {Availability, FormatRenderResourceCreateError(*Entry.Slot.GetFailure())};
		return {Availability};
	}

	auto FGlobalShaderMap::SetGeneration_RenderThread(
		const FRenderResourceGeneration& InGeneration,
		bool bForceShaderRecompile) -> void
	{
		if (Generation.Shader != InGeneration.Shader || Generation.Manual != InGeneration.Manual)
			InvalidateShaderMapPreparations(bForceShaderRecompile);
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
		PendingCompilation.reset();
		PublishedSections.clear();
		PublishedShaders.clear();
		Sections.clear();
		ClearShaderMapResourceCache();
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

#include "Shader/GlobalShader.h"
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

	} // namespace

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

	struct FGlobalShaderMap::FCompilation
	{
		struct FSection
		{
			std::string Name;
			std::vector<const FShaderType*> Types;
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
		const auto Profile = bCooked ? EShaderTargetProfile::Game : EShaderTargetProfile::EditorValidation;
		const auto Requests = GetRegisteredShaderRuntimeRequests(Profile, EShaderRuntimeRequestCategory::GlobalSet);
		if (const auto Duplicate = std::ranges::adjacent_find(Requests, {}, &FShaderRuntimeRequest::Name);
			Duplicate != Requests.end())
		{
			DURIN_ERROR("Global shader set '{}' is registered more than once.", Duplicate->Name);
			return;
		}
		for (const auto& Request : Requests)
		{
			auto& Section = Batch->Sections.emplace_back();
			Section.Name = Request.Name;
			auto& Types = Section.Types;
			auto& Options = Section.Options;
			auto Result = GetShaderRuntimeRequestBuildTypes(Request, Types);
			if (Result && bCooked) Result = LoadCookedShaderRuntimeRequest(Section.Name, Types, *Section.Output);
			else if (Result && (Result = BuildShaderMapCompileOptions(Types, nullptr, Options)))
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

	auto FGlobalShaderMap::ProcessCompilationResults_RenderThread() -> bool
	{
		if (!PendingCompilation) return false;
		for (const auto& Section : PendingCompilation->Sections)
			if (!Section.Job.IsComplete()) return false;
		auto Batch = std::move(PendingCompilation);
		decltype(PublishedShaders) Shaders;
		for (const auto& Section : Batch->Sections)
		{
			const auto& Output = *Section.Output;
			if (!Output)
			{
				if (Output.Status == EShaderBuildStatus::Error)
					DURIN_ERROR("Global shader set '{}' failed: {}", Section.Name, FormatShaderCompilerErrors(Output.Errors));
				return false;
			}
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			if (auto Result = ShaderMap->Initialize(Section.Types, Output, Section.Options); !Result)
			{
				DURIN_ERROR("Global shader set '{}' failed: {}", Section.Name, Result.error());
				return false;
			}
			for (const auto* Type : Section.Types)
				Shaders.try_emplace(Type, ShaderMap);
		}
		PublishedShaders.swap(Shaders);
		PublishedGeneration = Generation;
		return true;
	}

	auto FGlobalShaderMap::FindSection(const FGlobalShaderType& Type) const -> std::shared_ptr<FShaderMapBase>
	{
		const auto Found = PublishedShaders.find(&Type);
		return Found != PublishedShaders.end() ? Found->second : nullptr;
	}

	auto FGlobalShaderMap::SetGeneration_RenderThread(
		const FRenderResourceGeneration& InGeneration,
		bool bForceShaderRecompile) -> void
	{
		if (Generation.Shader != InGeneration.Shader || Generation.Manual != InGeneration.Manual)
			InvalidateShaderMapPreparations(bForceShaderRecompile);
		Generation = InGeneration;
		PublishedGeneration.Device = InGeneration.Device;
	}

	auto FGlobalShaderMap::ReleaseDeviceResources_RenderThread() -> void
	{
		ClearShaderMapResourceCache();
	}

	auto FGlobalShaderMap::Shutdown_RenderThread() -> void
	{
		PendingCompilation.reset();
		PublishedShaders.clear();
		ClearShaderMapResourceCache();
		Generation = {};
		PublishedGeneration = {};
	}

	auto GetGlobalShaderMap() -> FGlobalShaderMap&
	{
		static FGlobalShaderMap Map;
		return Map;
	}

} // namespace Durin

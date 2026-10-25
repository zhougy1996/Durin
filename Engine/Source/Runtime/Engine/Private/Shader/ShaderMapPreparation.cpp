#include "Shader/ShaderMapPreparation.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/Shader.h"
#include "Shader/ShaderData.h"
#include "RenderResourceCreation.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	struct FPreparedShaderMapState
	{
		uint64 Generation = 0;
		uint64 Epoch = 0;
		std::vector<const FShaderType*> Types;
		FShaderCompileOptions Options;
		std::mutex Mutex;
		std::optional<FShaderCompilerOutput> Output;
		FShaderBuildHandle Handle;
		EShaderBuildPriority Priority = EShaderBuildPriority::Low;
	};

	namespace
	{
		std::mutex PreparationMutex;
		std::vector<std::shared_ptr<FPreparedShaderMapState>> Preparations;
		std::atomic<uint64> PreparationEpoch = 1;
		std::atomic_bool ForcePreparation = false;

		auto MatchesPreparation(const FPreparedShaderMapState& State,
			const std::vector<const FShaderType*>& Types, const FShaderCompileOptions& Options) -> bool
		{
			if (State.Generation != GetShaderReloadGeneration() || State.Epoch != PreparationEpoch.load() || State.Types != Types
				|| State.Options.Target != Options.Target
				|| (Options.bForceRecompile && !State.Options.bForceRecompile)
				|| State.Options.Macros.size() != Options.Macros.size()) return false;
			for (size_t Index = 0; Index < Options.Macros.size(); ++Index)
				if (State.Options.Macros[Index].Name != Options.Macros[Index].Name
					|| State.Options.Macros[Index].Value != Options.Macros[Index].Value) return false;
			return true;
		}

		auto FindOrAddPreparation(std::vector<const FShaderType*> Types, FShaderCompileOptions Options,
			EShaderBuildPriority Priority, bool bCreate) -> std::shared_ptr<FPreparedShaderMapState>
		{
			std::vector<std::shared_ptr<FPreparedShaderMapState>> Retired;
			if (bCreate)
			{
				std::lock_guard Lock(PreparationMutex);
				std::erase_if(Preparations, [&](const auto& State) {
					if (State->Generation == GetShaderReloadGeneration() && State->Epoch == PreparationEpoch.load()) return false;
					Retired.push_back(State);
					return true;
				});
			}
			for (const auto& State : Retired) State->Handle.RequestCancel();
			std::shared_ptr<FPreparedShaderMapState> State;
			bool bSubmit = false;
			{
				std::lock_guard Lock(PreparationMutex);
				for (const auto& Existing : Preparations)
					if (MatchesPreparation(*Existing, Types, Options)) { State = Existing; break; }
				if (State && Priority < State->Priority)
				{
					State->Priority = Priority;
					State->Handle.SetPriority(Priority);
				}
				if (!State && bCreate)
				{
					State = std::make_shared<FPreparedShaderMapState>();
					State->Generation = GetShaderReloadGeneration();
					State->Epoch = PreparationEpoch.load();
					State->Priority = Priority;
					State->Types = std::move(Types);
					State->Options = std::move(Options);
					State->Handle = FShaderCompilingManager::Get().PrepareJobs(State->Options.VirtualShaderPath,
						State->Options, {.Priority = Priority}, [State](FShaderCompilerOutput Output) {
							std::lock_guard ResultLock(State->Mutex);
							State->Output = std::move(Output);
						});
					Preparations.push_back(State);
					bSubmit = true;
				}
			}
			if (bSubmit) FShaderCompilingManager::Get().SubmitJobs(State->Handle);
			return State;
		}
	}
	auto FShaderMapPreparation::Request(std::string_view Owner,
		std::initializer_list<std::string_view> Programs, EShaderBuildPriority Priority) -> FShaderMapPreparation
	{
		FShaderMapPreparation Result;
		Result.Priority = Priority;
		Result.Add(Owner, Programs);
		return Result;
	}

	auto FShaderMapPreparation::Add(std::string_view Owner,
		std::initializer_list<std::string_view> Names) -> void
	{
		const size_t FirstProgram = Programs.size();
		for (auto Name : Names) Programs.emplace_back(Owner, Name);
		Refresh(Generation != GetShaderReloadGeneration() || Epoch != PreparationEpoch.load() || !Error.empty()
			? 0 : FirstProgram);
	}

	auto FShaderMapPreparation::Refresh(size_t FirstProgram) -> void
	{
		if (FirstProgram == 0)
		{
			States.clear();
			Error.clear();
			Generation = GetShaderReloadGeneration();
			Epoch = PreparationEpoch.load();
		}
		if (Programs.empty()) return;
		if (GetShaderDataDomain() == EShaderDataDomain::Cooked) return;
		auto* Module = IShaderBuildModule::Get();
		if (!Module) { Error = "ShaderBuild is unavailable."; return; }
		for (size_t Index = FirstProgram; Index < Programs.size(); ++Index)
		{
			const auto& [Owner, Program] = Programs[Index];
			std::vector<const FShaderType*> Types;
			if (auto Result = GetShaderRuntimeRequestBuildTypes({.Owner = Owner, .Name = Program}, Types); !Result)
			{ Error = std::string(Result.error()); return; }
			std::ranges::sort(Types, {}, &FShaderType::GetName);
			FShaderCompileOptions Options;
			if (auto Result = BuildShaderMapCompileOptions(Types, nullptr, Options); !Result)
			{ Error = std::string(Result.error()); return; }
			Options.bForceRecompile = ForcePreparation.load();
			std::vector<FShaderMacroDefinition> Macros;
			if (auto Result = NormalizeShaderMacros(Options, Macros); !Result)
			{ Error = std::string(Result.error()); return; }
			Options.Macros = std::move(Macros);
			States.push_back(FindOrAddPreparation(std::move(Types), std::move(Options), Priority, true));
		}
	}

	auto FShaderMapPreparation::Poll() -> std::expected<bool, std::string>
	{
		if (Generation != GetShaderReloadGeneration() || Epoch != PreparationEpoch.load()) Refresh();
		if (!Error.empty()) return std::unexpected(Error);
		bool Ready = true;
		for (const auto& State : States)
		{
			std::lock_guard Lock(State->Mutex);
			if (!State->Output) { Ready = false; continue; }
			if (!*State->Output) return std::unexpected(State->Output->Errors.empty()
				? std::string("Shader preparation failed or was canceled.") : FormatShaderCompilerErrors(State->Output->Errors));
		}
		return Ready;
	}

	auto ClearShaderMapPreparations() -> void
	{
		InvalidateShaderMapPreparations(false);
		std::vector<std::shared_ptr<FPreparedShaderMapState>> Retired;
		{ std::lock_guard Lock(PreparationMutex); Retired.swap(Preparations); }
		for (const auto& State : Retired) State->Handle.RequestCancel();
		for (const auto& State : Retired) (void)State->Handle.Wait();
	}

	auto InvalidateShaderMapPreparations(bool bForceRecompile) -> void
	{
		ForcePreparation = bForceRecompile;
		++PreparationEpoch;
	}

	auto InitializeShaderMapFromTypes(FShaderMapBase& Map,
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompileOptions& CompileOptions) -> FShaderCompletion
	{
		FShaderOperationResult Result;
		FShaderCompileOptions EffectiveCompileOptions;
		if (!(Result = BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions)))
		{
			Map.Reset();
			return Result;
		}

		std::vector<const FShaderType*> StableTypes(ShaderTypes.begin(), ShaderTypes.end());
		std::ranges::sort(StableTypes, {}, &FShaderType::GetName);
		std::vector<FShaderMacroDefinition> Macros;
		if (!(Result = NormalizeShaderMacros(EffectiveCompileOptions, Macros))) return Result;
		EffectiveCompileOptions.Macros = std::move(Macros);
		const bool bCaptureOnly = FPreparedShaderMapScope::IsActive() && !FPreparedShaderMapScope::AllowsAsyncRequests();
		const bool bAsyncDemand = IsInRenderingThread() && !bCaptureOnly;
		if (bAsyncDemand && !IShaderBuildModule::Get())
			return std::unexpected(std::string("ShaderBuild module is unavailable."));
		auto Prepared = FindOrAddPreparation(std::move(StableTypes), EffectiveCompileOptions,
			EShaderBuildPriority::Normal, bAsyncDemand);
		if (Prepared)
		{
			std::lock_guard Lock(Prepared->Mutex);
			if (Prepared->Output && *Prepared->Output)
			{
				FShaderCompilerOutput Output{.Status = EShaderBuildStatus::Ok};
				for (const auto* Type : ShaderTypes)
				{
					auto Found = std::ranges::find_if(Prepared->Output->CompiledShaders, [Type](const auto& Code) {
						return Code.SourceEntryPoint == Type->GetEntryPoint() && Code.Frequency == Type->GetFrequency();
					});
					if (Found == Prepared->Output->CompiledShaders.end())
						return std::unexpected(std::format("Material shader map is missing type '{}'.", (std::string{})));
					Output.CompiledShaders.push_back(*Found);
				}
				return Map.Initialize(ShaderTypes, Output, EffectiveCompileOptions);
			}
			if (Prepared->Output) return {Prepared->Output->Status, FormatShaderCompilerErrors(Prepared->Output->Errors)};
		}
		if (bCaptureOnly || bAsyncDemand)
		{
			FPreparedShaderMapScope::RecordPendingRequest();
			return std::unexpected(std::string("Shader compilation has not completed."));
		}
		auto* Module = IShaderBuildModule::Get();
		if (!Module)
		{
			Map.Reset();
			return std::unexpected(std::string("ShaderBuild module is unavailable."));
		}
		FShaderCompilerOutput Output = FShaderCompilingManager::Get().CompileMounted(EffectiveCompileOptions.VirtualShaderPath, EffectiveCompileOptions);
		if (!Output)
		{
			Map.Reset();
			return {Output.Status, FormatShaderCompilerErrors(Output.Errors)};
		}

		return Map.Initialize(ShaderTypes, std::move(Output), EffectiveCompileOptions);
	}

}

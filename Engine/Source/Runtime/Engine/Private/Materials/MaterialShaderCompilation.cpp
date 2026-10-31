#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "MaterialPreparedProgram.h"

#if DURIN_WITH_EDITOR
	#include "RequestOwnerInternal.h"
	#include "MaterialTranslation.h"
	#include "MaterialTranslationCodec.h"
	#include "Shader/IShaderBuildModule.h"
	#include "Threading/Task.h"
#endif

namespace Durin
{
	using namespace MaterialBuildPrivate;
	FMaterialCompilation::FMaterialCompilation() = default;
	FMaterialCompilation::~FMaterialCompilation() = default;
	namespace {
		std::mutex CompletionHookMutex;
		std::function<void(EMaterialQualityLevel, bool)> CompletionHook;
	}
	auto MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void
	{ std::lock_guard Lock(CompletionHookMutex); CompletionHook = std::move(Hook); }
	namespace
	{
		auto CanceledMaterial() -> FMaterialCompilerResult
		{ return {.Status = EMaterialProgramStatus::Canceled}; }
		auto FailedMaterial(std::string Diagnostic = {}) -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Message = Diagnostic.empty() ? "Material compile admission rejected." : std::move(Diagnostic)}}}; }
	}


#if DURIN_WITH_EDITOR


	auto MaterialBuildPrivate::FMaterialCompilation::Compile(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return FShaderCompilingManager::Get().Submit(std::move(Input), std::move(Options), std::move(Completion), false); }
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(const MIR::FCompilerInput& Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{
		auto Borrowed = std::shared_ptr<const MIR::FCompilerInput>(&Input, [](const auto*) {});
		return FShaderCompilingManager::Get().Submit(std::move(Borrowed), std::move(Options), std::move(Completion), true);
	}
	auto FShaderCompilingManager::Submit(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion, bool CopyInput)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{
		CleanupFinishedShaderMaps();
		if (!Input || !Completion) return std::unexpected(EMaterialCompileAdmissionError::Unavailable);
		// Admission precedes encoding, normalization or any input/product copy.
		FShaderMapPtr ShaderMap;
		std::unique_lock AdmissionLock(MaterialMutex);
			if (!Accepting) return std::unexpected(EMaterialCompileAdmissionError::Closed);
			if (CompilingShaderMaps.size() >= MaterialCompileMaxConcurrentRequests) return std::unexpected(EMaterialCompileAdmissionError::Capacity);
			if (EstimateTranslationCaptureBytes(*Input) > TranslationInputBytes)
				return std::unexpected(EMaterialCompileAdmissionError::InputTooLarge);
			ShaderMap = Options.Request ? std::move(Options.Request) : std::make_shared<FMaterialCompilation>();
			require(ShaderMap->CompilingId == 0);
			static std::atomic_uint64_t NextCompilingId = 1;
			ShaderMap->CompilingId = NextCompilingId.fetch_add(1);
			ShaderMap->Owner = std::make_shared<DerivedData::FRequestOwner>();
			ShaderMap->Input = CopyInput ? std::make_shared<const MIR::FCompilerInput>(*Input) : std::move(Input);
			ShaderMap->EncodedInput = std::move(Options.EncodedInput);
			ShaderMap->Force = Options.bForceRecompile; ShaderMap->Cancellation = std::move(Options.Cancellation);
			ShaderMap->Completion = std::move(Completion);
			ShaderMap->Inline = !IsTaskSchedulerRunning();
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		CompilingShaderMaps.push_back(ShaderMap);
		AdmissionLock.unlock();
		auto Handle = ShaderMap;
		LaunchTask(ShaderMap, [ShaderMap] { FMaterialCompilation::Start(ShaderMap); });
		return Handle;
	}

	auto FShaderCompilingManager::LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void
	{
		if (ShaderMap->Inline) {
			try { if (IsBuildCanceled(ShaderMap)) FMaterialCompilation::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
			catch (...) { FMaterialCompilation::CompleteCompilation(ShaderMap, FailedMaterial()); }
			return;
		}
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		ETaskPriority Priority;
		{ std::lock_guard Lock(ShaderMap->Mutex); Priority = ShaderMap->HighPriority ? ETaskPriority::High : ETaskPriority::Normal; }
		auto Dispatch = Private::LaunchCancelableTaskWithCompletion("MaterialCompileStage",
			[ShaderMap, Work = std::move(Work)](const FTaskCancellationToken& Token) {
				try { if (Token.IsCancellationRequested() || IsBuildCanceled(ShaderMap)) FMaterialCompilation::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
				catch (...) { FMaterialCompilation::CompleteCompilation(ShaderMap, FailedMaterial()); }
			}, [ShaderMap](ETaskState Terminal) {
				if (Terminal != ETaskState::Succeeded) FMaterialCompilation::CompleteCompilation(ShaderMap, FailedMaterial());
			}, {.CancellationToken = ShaderMap->CpuCancellation.GetToken(), .Priority = Priority});
		{
			std::lock_guard Lock(ShaderMap->Mutex);
			auto Task = std::move(Dispatch);
			Durin::DerivedData::Private::FOwnerAccess::TrackTask(*ShaderMap->Owner, std::move(Task));
		}
	}




	auto FShaderCompilingManager::IsBuildCanceled(FShaderMapPtr ShaderMap) -> bool
	{
		std::vector<FCancellationToken> Tokens;
		{
			std::lock_guard Lock(Get().MaterialMutex);
			if (ShaderMap->BuildCancellation.IsCancellationRequested()) return true;
			Tokens.push_back(ShaderMap->Cancellation);
			for (const auto& Subscriber : ShaderMap->CompileDependencies) Tokens.push_back(Subscriber->Cancellation);
		}
		// Observers can be caller code. Evaluate outside the manager lock, then
		// recheck latched flags under lock so a newly joined subscriber also wins.
		for (const auto& Token : Tokens) (void)Token.IsCancelled();
		std::lock_guard Lock(Get().MaterialMutex);
		if (!ShaderMap->Cancellation.IsCancellationRequested()) return false;
		for (const auto& Subscriber : ShaderMap->CompileDependencies)
			if (!Subscriber->Cancellation.IsCancellationRequested()) return false;
		ShaderMap->BuildCancellation.Cancel(); return true;
	}


	auto FShaderCompilingManager::FindOrAddCompilingShaderMap(FShaderMapPtr ShaderMap)
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		FShaderMapPtr Shared;
		{
			std::lock_guard Lock(MaterialMutex);
			// Cache lookup and joining observe the same handoff as completion.
			if (!ShaderMap->Force)
				if (auto Cached = FMaterialShaderMap::FindId(ShaderMap->ShaderMapId))
					return std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{
						.Status = EMaterialProgramStatus::Ok, .ActiveCollections = Cached->CollectionLayouts,
						.ShaderMap = Cached});
			auto& Programs = CompilingPrograms[ShaderMap->Force];
			const auto Entry = Programs.find(ShaderMap->ShaderMapId);
			auto Existing = Entry == Programs.end() ? FShaderMapPtr{} : Entry->second.lock();
			if (Existing && !Existing->BuildCancellation.IsCancellationRequested()) {
				ShaderMap->SingleFlight = true;
				ProgramJoins.fetch_add(1);
				Durin::DerivedData::Private::FOwnerAccess::TrackDependency(*ShaderMap->Owner, {.IsFinished = [Owner = Existing->Owner] { return Owner->Poll(); }, .Wait = [Owner = Existing->Owner] { return Owner->Wait(); }, .RequestCancel = [] {}});
				Existing->CompileDependencies.push_back(ShaderMap);
				Shared = std::move(Existing);
			}
			else Programs[ShaderMap->ShaderMapId] = ShaderMap;
		}
		if (Shared) {
			bool HighPriority;
			{ std::lock_guard Lock(ShaderMap->Mutex); HighPriority = ShaderMap->HighPriority; }
			if (HighPriority) PrioritizeCompilation(Shared);
		}
		return {};
	}

	auto FShaderCompilingManager::FinalizeCompilation(FShaderMapPtr ShaderMap,
		std::shared_ptr<const FMaterialCompilerResult> Program, bool RetainedHit,
		std::vector<FShaderMapPtr>& Subscribers, std::function<void(FMaterialBuildResult)>& Completion) -> FMaterialBuildResult
	{
		std::lock_guard StateLock(MaterialMutex);
		std::lock_guard Lock(ShaderMap->Mutex);
		if (ShaderMap->ResultReady) return {};
		ShaderMap->ResultReady = true;
		Completion = std::move(ShaderMap->Completion);
		Subscribers = std::move(ShaderMap->CompileDependencies);
		// Publish before removing the in-flight identity, under the lookup lock.
		if (*Program && !RetainedHit && !ShaderMap->SingleFlight) {
			const auto Canonical = FMaterialShaderMap::Register(Program->ShaderMap, ShaderMap->Force);
			if (Canonical != Program->ShaderMap) {
				auto Result = std::make_shared<FMaterialCompilerResult>(*Program);
				Result->ShaderMap = Canonical;
				Program = std::move(Result);
			}
		}
		auto& Programs = CompilingPrograms[ShaderMap->Force];
		const auto Existing = Programs.find(ShaderMap->ShaderMapId);
		if (Existing != Programs.end() && Existing->second.lock() == ShaderMap) Programs.erase(Existing);
		return {.Program = std::move(Program), .RetainedHit = RetainedHit,
			.SingleFlight = ShaderMap->SingleFlight};
	}

	auto FShaderCompilingManager::FindCompilingShaderMap(FShaderMapPtr Request) const -> FShaderMapPtr
	{
		std::lock_guard Lock(MaterialMutex);
		const auto Found = CompilingPrograms[Request->Force].find(Request->GetShaderMapId());
		if (Found != CompilingPrograms[Request->Force].end()) {
			auto Producer = Found->second.lock();
			// A canceled producer can be replaced under the same identity. Only a
			// current dependency may affect its replacement's work or priority.
			if (Producer && (Producer == Request || std::ranges::contains(Producer->CompileDependencies, Request))) return Producer;
		}
		return Request;
	}

	auto FShaderCompilingManager::PrioritizeCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		auto ShaderMap = FindCompilingShaderMap(Handle);
		std::shared_ptr<FShaderBuildHandle> Jobs;
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->HighPriority = true; Jobs = ShaderMap->ShaderJobs; Owner = ShaderMap->Owner; }
		if (Owner) Owner->SetPriority(DerivedData::ERequestPriority::High);
		if (Jobs) Jobs->SetPriority(EShaderBuildPriority::High);
	}
	auto FShaderCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		Handle->Cancellation.Cancel();
		auto ShaderMap = FindCompilingShaderMap(Handle);
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(ShaderMap->Mutex); Owner = ShaderMap->Owner; }
		if (Owner && IsBuildCanceled(ShaderMap)) { ShaderMap->CpuCancellation.RequestCancellation(); Owner->RequestCancel(); }
	}
	auto FShaderCompilingManager::IsCompiling(const FMaterialProgramCompileHandle& Handle) const -> bool
	{
		if (!Handle) return false;
		std::lock_guard Lock(Handle->Mutex);
		return !Handle->ResultReady || (Handle->Owner && !Handle->Owner->Poll());
	}
	auto FShaderCompilingManager::FinishCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		PrioritizeCompilation(Handle);
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Handle->Mutex); Owner = Handle->Owner; }
		if (Owner) require(Owner->Wait() == DerivedData::ERequestWaitResult::Completed);
		require(!IsCompiling(Handle));
		CleanupFinishedShaderMaps();
	}
	auto FShaderCompilingManager::CleanupFinishedShaderMaps() -> void
	{
		std::vector<FShaderMapPtr> Finished;
		{ std::lock_guard Lock(MaterialMutex); Finished = CompilingShaderMaps; }
		std::erase_if(Finished, [this](const auto& ShaderMap) { return IsCompiling(ShaderMap); });
		// Destroy request owners outside the manager lock: destruction propagates cancellation.
		std::vector<std::shared_ptr<DerivedData::FRequestOwner>> RetiredOwners;
		std::lock_guard Lock(MaterialMutex);
		for (const auto& ShaderMap : Finished) {
			// Complete missing callbacks atomically with cleanup; no separate scan or fanout.
			const auto It = RequestedShaderMaps.find({ShaderMap->CanonicalIdentity, ShaderMap->Force});
			if (It != RequestedShaderMaps.end() && It->second == ShaderMap) {
				const bool Canceled = ShaderMap->Cancellation.IsCancellationRequested();
				for (const auto& Consumer : ShaderMap->CompilingMaterialDependencies)
					MaterialResults.push_back(MakeMaterialResult(Consumer, {}, nullptr, Canceled));
				(Canceled ? MaterialDiagnostics.CanceledRequests : MaterialDiagnostics.FailedRequests) += ShaderMap->CompilingMaterialDependencies.size();
				ShaderMap->CompilingMaterialDependencies.clear();
				RequestedShaderMaps.erase(It);
			}
			std::lock_guard RecordLock(ShaderMap->Mutex);
			ShaderMap->Input.reset(); ShaderMap->EncodedInput.reset(); ShaderMap->Translation.reset();
			ShaderMap->ShaderJobs.reset();
			CapturedMaterialBytes -= ShaderMap->CapturedBytes; ShaderMap->CapturedBytes = 0;
			ShaderMap->DiagnosticInput.reset(); ShaderMap->Origins.clear();
			RetiredOwners.push_back(std::move(ShaderMap->Owner));
			std::erase(CompilingShaderMaps, ShaderMap);
		}
	}
	auto FShaderCompilingManager::FinishMaterialCompilation(std::span<const FWeakObjectPtr> Owners) -> void
	{
		for (;;) {
			std::vector<FShaderMapPtr> Records;
			{
				std::lock_guard Lock(MaterialMutex);
				Records = CompilingShaderMaps;
				if (!Owners.empty()) std::erase_if(Records, [&](const auto& Map) {
					return !std::ranges::any_of(Map->Origins, [&](auto Origin) {
						return std::ranges::any_of(Owners, [&](auto Owner) { return Origin.GetKey() == Owner.GetKey(); });
					});
				});
			}
			if (Records.empty()) return;
			for (const auto& ShaderMap : Records) {
				FinishCompilation(ShaderMap);
			}
			// A completion callback may have submitted another compile while we waited.
		}
	}
	auto FShaderCompilingManager::CancelMaterialCompilation() -> void
	{
		std::vector<FShaderMapPtr> Records;
		{ std::lock_guard Lock(MaterialMutex); Records = CompilingShaderMaps; }
		for (const auto& ShaderMap : Records) CancelCompilation(ShaderMap);
		FinishMaterialCompilation();
	}

	auto MaterialBuildPrivate::CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult
	{
		FMaterialBuildResult Result;
		auto& Manager = FShaderCompilingManager::Get();
		auto Submitted = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = Force},
			[&](FMaterialBuildResult Value) { Result = std::move(Value); });
		if (!Submitted) return {.Program = std::make_shared<const FMaterialCompilerResult>(FailedMaterial())};
		Manager.FinishCompilation(*Submitted);
		return Result;
	}
#else
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(std::shared_ptr<const MIR::FCompilerInput>, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return std::unexpected(EMaterialCompileAdmissionError::Unavailable); }
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(const MIR::FCompilerInput&, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return std::unexpected(EMaterialCompileAdmissionError::Unavailable); }
	auto FShaderCompilingManager::FinishCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FShaderCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FShaderCompilingManager::PrioritizeCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FShaderCompilingManager::IsCompiling(const FMaterialProgramCompileHandle&) const -> bool { return false; }
	auto FShaderCompilingManager::CleanupFinishedShaderMaps() -> void {}
	auto FShaderCompilingManager::FinishMaterialCompilation(std::span<const FWeakObjectPtr> Owners) -> void {}
	auto FShaderCompilingManager::CancelMaterialCompilation() -> void {}
	auto MaterialBuildPrivate::CompileMaterialInput(const MIR::FCompilerInput&, bool) -> FMaterialBuildResult
	{ return {.Program = std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{
		.Diagnostics = {{.Message = "Material has no authored program."}}})}; }
#endif

	auto FShaderCompilingManager::GetNumRemainingMaterialShaderMaps() const -> uint64
	{
#if DURIN_WITH_EDITOR
		std::lock_guard Lock(MaterialMutex);
		return std::ranges::count_if(CompilingShaderMaps, [this](const auto& Map) { return Map->Origins.empty() && IsCompiling(Map); });
#else
		return 0;
#endif
	}
	auto MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats
	{ return {FShaderCompilingManager::Get().ProgramJoins.load()}; }
#if DURIN_WITH_EDITOR
	auto MaterialBuildPrivate::FMaterialCompilation::Start(std::shared_ptr<FMaterialCompilation> ShaderMap) -> void
	{
		auto Admitted = MaterialBuildPrivate::SubmitTranslation(ShaderMap->Input, ShaderMap->Force, *ShaderMap->Owner,
			[ShaderMap](FMaterialTranslatedProgram Product) {
				try { ProcessTranslation(ShaderMap, std::move(Product)); }
				catch (...) { CompleteCompilation(ShaderMap, {.Diagnostics = {{
					.Message = "Material compile admission was rejected."}}}); }
			}, true, ShaderMap->EncodedInput,
			ShaderMap->Inline ? DerivedData::EBuildCacheExecution::Inline : DerivedData::EBuildCacheExecution::Task);
		if (!Admitted) CompleteCompilation(ShaderMap, {.Diagnostics = {{
			.Message = Admitted.error().Description}}});
	}
	auto MaterialBuildPrivate::FMaterialCompilation::ProcessTranslation(std::shared_ptr<FMaterialCompilation> ShaderMap, FMaterialTranslatedProgram Translation) -> void
	{
		if (FShaderCompilingManager::IsBuildCanceled(ShaderMap)) return CompleteCompilation(ShaderMap, FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled});
		std::shared_ptr<const FMaterialCompilerResult> Retained;
		ShaderMap->Sources = std::move(Translation.Sources);
		if (!Translation) return CompleteCompilation(ShaderMap, FinalizeMaterialShaderMap(std::move(Translation), {}));
		{ std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->ShaderMapId = Translation.Identity; }
		Retained = FShaderCompilingManager::Get().FindOrAddCompilingShaderMap(ShaderMap);
		if (Retained) return CompleteCompilation(ShaderMap, {}, std::move(Retained));
		if (ShaderMap->SingleFlight) return;
		ShaderMap->Translation = std::make_unique<FMaterialTranslatedProgram>(std::move(Translation));
		ShaderMap->CompilationBegin = std::chrono::steady_clock::now();
		FShaderCompilingManager::SubmitCompileJobs(ShaderMap);
	}

	auto MaterialBuildPrivate::FMaterialCompilation::CompleteCompilation(std::shared_ptr<FMaterialCompilation> ShaderMap, FMaterialCompilerResult Result,
		std::shared_ptr<const FMaterialCompilerResult> Retained) -> void
	{
		DerivedData::FRequestBarrier DeliveryBarrier(*ShaderMap->Owner);
		std::function<void(MaterialBuildPrivate::FMaterialBuildResult)> Callback;
		std::vector<std::shared_ptr<FMaterialCompilation>> Subscribers;
		// Cache hits and shared subscribers bind once, before byte-limit checks.
		if (Retained) Retained = MaterialBuildPrivate::BindMaterialProgramCollections(std::move(Retained), ShaderMap->Input->Collections, &ShaderMap->Input->Environment);
		if ((Result && MaterialBuildPrivate::GetMaterialProgramRetainedBytes(Result) > MaterialCompileMaxResultBytes)
			|| (Retained && *Retained && MaterialBuildPrivate::GetMaterialProgramRetainedBytes(*Retained) > MaterialCompileMaxResultBytes)) {
			Retained.reset();
			Result = {.Diagnostics = {{.Message = "Compiled material result exceeds the retained-result byte limit."}}};
		}
		const auto Quality = Retained ? Retained->Quality : Result.Quality;
		const bool RetainedHit = bool(Retained) && !ShaderMap->SingleFlight;
		auto Program = Retained ? std::move(Retained) : std::make_shared<const FMaterialCompilerResult>(std::move(Result));
		auto Build = FShaderCompilingManager::Get().FinalizeCompilation(
			ShaderMap, std::move(Program), RetainedHit, Subscribers, Callback);
		if (!Build.Program || !Callback) return;
		std::function<void(EMaterialQualityLevel, bool)> Hook;
		{ std::lock_guard Lock(CompletionHookMutex); Hook = CompletionHook; }
		for (const auto& Subscriber : Subscribers) {
			auto Program = Build.Program;
			if (Program && !*Program) {
				auto Qualified = std::make_shared<FMaterialCompilerResult>(*Program);
				for (auto& Diagnostic : Qualified->Diagnostics) {
					const auto Origin = std::ranges::find(ShaderMap->Sources, Diagnostic.NodeId, [](const auto& Location) { return Location.Source.NodeId; });
					if (Origin == ShaderMap->Sources.end()) continue;
					const auto Location = std::ranges::find(Subscriber->Sources, Origin->Source.ExpressionIndex, [](const auto& Location) { return Location.Source.ExpressionIndex; });
					if (Location == Subscriber->Sources.end()) continue;
					const auto& Source = Location->Source;
					Diagnostic.NodeId = Source.NodeId; Diagnostic.PortId = Source.PortId;
					Diagnostic.FunctionAssetPath = Source.FunctionAssetPath; Diagnostic.CallPath = Source.CallPath;
				}
				Program = std::move(Qualified);
			}
			// A subscriber callback cannot prevent another subscriber from retiring.
			try { FShaderCompilingManager::LaunchTask(Subscriber, [Subscriber, Program = std::move(Program)] { CompleteCompilation(Subscriber, {}, Program); }); } catch (...) {}
		}
		if (Hook) Hook(Quality, true);
		if (ShaderMap->Cancellation.IsCancelled()) { Build.Program = std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled}); Build.RetainedHit = false; }
		Build.Sources = std::move(ShaderMap->Sources);
		Callback(std::move(Build));
		if (Hook) Hook(Quality, false);
	}

#endif

	auto MaterialBuildPrivate::FinalizeMaterialShaderMap(FMaterialTranslatedProgram Translation,
		FShaderCompilerOutput Output) -> FMaterialCompilerResult
	{
		auto Artifact = std::make_shared<FMaterialShaderMap>();
		Artifact->ActiveParameters = std::move(Translation.ActiveParameters);
		Artifact->Layout = std::move(Translation.Layout);
		Artifact->Requirements = Translation.Requirements;
		Artifact->Dependencies = std::move(Translation.Dependencies);
		Artifact->ShaderMapId = Translation.Identity;
		Artifact->CompilerIdentity = std::move(Translation.CompilerIdentity);
		Artifact->Target = std::move(Translation.Target);
		Artifact->PassContractVersion = Translation.PassContractVersion;
		Artifact->RequiredStages = Translation.RequiredStages;
		Artifact->DebugData = std::make_shared<const FMaterialProgramDebugData>(FMaterialProgramDebugData{
			.IR = std::move(Translation.IR), .GeneratedSource = std::move(Translation.GeneratedSource)});
		FMaterialCompilerResult Result{
			.Quality = Translation.Quality, .FeatureLevel = Translation.FeatureLevel,
			.StaticBools = std::move(Translation.StaticBools), .ActiveCollections = std::move(Translation.ActiveCollections),
			.ShaderMap = Artifact,
			.Diagnostics = std::move(Translation.Diagnostics)};
		if (Translation.Status == EMaterialProgramStatus::Canceled || Output.Status == EShaderBuildStatus::Canceled)
			return {.Status = EMaterialProgramStatus::Canceled};
		if (!Translation) return Result;
		for (const auto& Diagnostic : Output.Errors) {
			if (Result.Diagnostics.size() >= MaterialProgramMaxDiagnosticCount) break;
			Result.Diagnostics.push_back({.Message = Diagnostic.GetErrorString()});
		}
		if (!Output) return Result;

		const auto Target = Artifact->Target == "metal-library-1" ? MetalLibraryShaderTarget
			: Artifact->Target == "metal-msl-2.0" ? MetalShaderTarget : VulkanShaderTarget;
		const auto Reflection = std::ranges::all_of(Output.CompiledShaders,
			[&](const auto& Shader) { return Shader.Target == Target; })
			? ValidateMaterialCompiledStages(Output.CompiledShaders,
				Artifact->Layout, Translation.ResourceLimits, Result.ActiveCollections, Artifact->RequiredStages)
			: FMaterialLayoutValidationResult{.Error = EMaterialLayoutError::InvalidReflection};
		if (!Reflection)
		{
			Result.Diagnostics.push_back({.Message = std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Reflection.Error), Reflection.ParameterId.IsValid() ? std::format(" [parameter {}]", Reflection.ParameterId.ToString()) : "", Reflection.FieldIndex)});
			return Result;
		}
		Artifact->CompiledShaders = std::move(Output.CompiledShaders);
		Artifact->CompileTime = Translation.CompileTime;
		Artifact->SetCollectionLayouts(Result.ActiveCollections);
		Result.Status = EMaterialProgramStatus::Ok;
		return Result;
	}

}

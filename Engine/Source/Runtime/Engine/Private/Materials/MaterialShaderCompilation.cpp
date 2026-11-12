#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "MaterialPreparedProgram.h"

#if DURIN_WITH_EDITOR
	#include "RequestOwnerAccess.h"
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
		-> FMaterialProgramCompileHandle
	{ return FShaderCompilingManager::Get().Submit(std::move(Input), std::move(Options), std::move(Completion), false); }
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(const MIR::FCompilerInput& Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> FMaterialProgramCompileHandle
	{
		auto Borrowed = std::shared_ptr<const MIR::FCompilerInput>(&Input, [](const auto*) {});
		return FShaderCompilingManager::Get().Submit(std::move(Borrowed), std::move(Options), std::move(Completion), true);
	}
	auto FShaderCompilingManager::Submit(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion, bool CopyInput)
		-> FMaterialProgramCompileHandle
	{
		CleanupFinishedShaderMaps();
		if (!Input || !Completion) return {};
		// All accepted requests join the task runtime; worker concurrency does not reject submissions.
		FShaderMapPtr ShaderMap;
		std::unique_lock AdmissionLock(MaterialMutex);
			if (!Accepting) return {};
			ShaderMap = Options.Request ? std::move(Options.Request) : std::make_shared<FMaterialCompilation>();
			require(ShaderMap->CompilingId == 0);
			static std::atomic_uint64_t NextCompilingId = 1;
			ShaderMap->CompilingId = NextCompilingId.fetch_add(1);
			ShaderMap->Owner = std::make_shared<DerivedData::FRequestOwner>();
			ShaderMap->BuildOwner = std::make_shared<DerivedData::FRequestOwner>();
			ShaderMap->Input = CopyInput ? std::make_shared<const MIR::FCompilerInput>(*Input) : std::move(Input);
			ShaderMap->EncodedInput = std::move(Options.EncodedInput);
			ShaderMap->Force = Options.bForceRecompile; ShaderMap->IsCancellationRequested = std::move(Options.IsCancellationRequested);
			ShaderMap->Completion = std::move(Completion);
			ShaderMap->Inline = !IsTaskSchedulerRunning();
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		static_cast<DerivedData::IRequestOwner&>(*ShaderMap->Owner).Begin(static_cast<DerivedData::IRequest*>(*ShaderMap->BuildOwner));
		CompilingShaderMaps.push_back(ShaderMap);
		AdmissionLock.unlock();
		auto Handle = ShaderMap;
		LaunchTask(ShaderMap, [ShaderMap] { FMaterialCompilation::Start(ShaderMap); });
		return Handle;
	}

	auto FShaderCompilingManager::LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void
	{
		if (ShaderMap->Inline) {
			try { if (PollBuildCancellation(ShaderMap)) FMaterialCompilation::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
			catch (...) { FMaterialCompilation::CompleteCompilation(ShaderMap, FailedMaterial()); }
			return;
		}
		ShaderMap->BuildOwner->LaunchTask("MaterialCompileStage", [ShaderMap, Work = std::move(Work)] {
			try { if (PollBuildCancellation(ShaderMap)) FMaterialCompilation::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
			catch (...) { FMaterialCompilation::CompleteCompilation(ShaderMap, FailedMaterial()); }
		});
	}




	auto FShaderCompilingManager::PollCancellation(FShaderMapPtr Request) -> bool
	{
		bool Canceled = Request->Cancellation.IsCanceled();
		if (!Canceled && Request->IsCancellationRequested) {
			// Only the external observer runs under this lock. Propagation may invoke
			// callbacks, so it must use the common entry point after releasing it.
			std::lock_guard Lock(Request->CancellationMutex);
			Canceled = Request->Cancellation.IsCanceled() || Request->IsCancellationRequested();
		}
		if (Canceled) Get().CancelCompilation(Request);
		return Request->Cancellation.IsCanceled();
	}

	auto FShaderCompilingManager::PollBuildCancellation(FShaderMapPtr ShaderMap) -> bool
	{
		std::vector<FShaderMapPtr> Requests;
		{
			std::lock_guard Lock(Get().MaterialMutex);
			if (ShaderMap->BuildCancellation.IsCanceled()) return true;
			Requests.push_back(ShaderMap);
			for (const auto& Subscriber : ShaderMap->CompileDependencies) Requests.push_back(Subscriber);
		}
		for (const auto& Request : Requests) (void)PollCancellation(Request);
		return ShaderMap->BuildCancellation.IsCanceled();
	}

	auto FShaderCompilingManager::FindOrAddCompilingShaderMap(FShaderMapPtr ShaderMap)
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		auto Owner = ShaderMap->Owner;
		DerivedData::FRequestBarrier Publication(*Owner);
		std::shared_ptr<DerivedData::FRequestOwner> Shared;
		DerivedData::ERequestPriority Priority = DerivedData::ERequestPriority::Normal;
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
			if (Existing && !Existing->BuildCancellation.IsCanceled()) {
				ShaderMap->SingleFlight = true;
				ProgramJoins.fetch_add(1);
				Shared = Existing->BuildOwner;
				static_cast<DerivedData::IRequestOwner&>(*Owner).Begin(static_cast<DerivedData::IRequest*>(*Shared));
				static_cast<DerivedData::IRequestOwner&>(*Owner).End(static_cast<DerivedData::IRequest*>(*ShaderMap->BuildOwner));
				Priority = Owner->GetPriority();
				Existing->CompileDependencies.push_back(ShaderMap);
			}
			else Programs[ShaderMap->ShaderMapId] = ShaderMap;
		}
		if (Shared && Priority == DerivedData::ERequestPriority::High) Shared->SetPriority(Priority);
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
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Handle->Mutex); Owner = Handle->Owner; }
		if (Owner) Owner->SetPriority(DerivedData::ERequestPriority::High);
	}
	auto FShaderCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		Handle->Cancellation.Cancel();
		auto ShaderMap = FindCompilingShaderMap(Handle);
		{
			// Joining and the last-subscriber cancellation share the same lock.
			std::lock_guard Lock(MaterialMutex);
			if (ShaderMap->BuildCancellation.IsCanceled()) return;
			if (!ShaderMap->Cancellation.IsCanceled()) return;
			for (const auto& Subscriber : ShaderMap->CompileDependencies)
				if (!Subscriber->Cancellation.IsCanceled()) return;
			ShaderMap->BuildCancellation.Cancel();
		}
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(ShaderMap->Mutex); Owner = ShaderMap->BuildOwner; }
		// Owner propagation includes shader jobs. Accepted CPU tasks still retire.
		if (Owner) Durin::DerivedData::Private::FOwnerAccess::RequestCancel(*Owner);
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
		(void)PollBuildCancellation(FindCompilingShaderMap(Handle));
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Handle->Mutex); Owner = Handle->Owner; }
		if (Owner) Owner->Wait();
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
			if (!ShaderMap->CompilingMaterialDependencies.empty()) {
				const bool Canceled = ShaderMap->Cancellation.IsCanceled();
				for (const auto& Consumer : ShaderMap->CompilingMaterialDependencies)
					MaterialResults.push_back(MakeMaterialResult(Consumer, {}, nullptr, Canceled));
				(Canceled ? MaterialDiagnostics.CanceledRequests : MaterialDiagnostics.FailedRequests) += ShaderMap->CompilingMaterialDependencies.size();
				ShaderMap->CompilingMaterialDependencies.clear();
				if (It != RequestedShaderMaps.end() && It->second == ShaderMap) RequestedShaderMaps.erase(It);
			}
			std::lock_guard RecordLock(ShaderMap->Mutex);
			ShaderMap->Input.reset(); ShaderMap->EncodedInput.reset(); ShaderMap->Translation.reset();
			ShaderMap->ShaderJobs.reset();
			ShaderMap->DiagnosticInput.reset(); ShaderMap->Origins.clear();
			RetiredOwners.push_back(std::move(ShaderMap->Owner));
			RetiredOwners.push_back(std::move(ShaderMap->BuildOwner));
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
	}

	auto MaterialBuildPrivate::CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult
	{
		FMaterialBuildResult Result;
		auto& Manager = FShaderCompilingManager::Get();
		auto Submitted = MaterialBuildPrivate::FMaterialCompilation::Compile(Input, {.bForceRecompile = Force},
			[&](FMaterialBuildResult Value) { Result = std::move(Value); });
		if (!Submitted) return {.Program = std::make_shared<const FMaterialCompilerResult>(FailedMaterial())};
		Manager.FinishCompilation(Submitted);
		return Result;
	}
#else
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(std::shared_ptr<const MIR::FCompilerInput>, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> FMaterialProgramCompileHandle
	{ return {}; }
	auto MaterialBuildPrivate::FMaterialCompilation::Compile(const MIR::FCompilerInput&, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> FMaterialProgramCompileHandle
	{ return {}; }
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
		MaterialBuildPrivate::SubmitTranslation(ShaderMap->Input, ShaderMap->Force, *ShaderMap->BuildOwner,
			[ShaderMap](FMaterialTranslatedProgram Product) {
				try { ProcessTranslation(ShaderMap, std::move(Product)); }
				catch (...) { CompleteCompilation(ShaderMap, {.Diagnostics = {{
					.Message = "Material compile admission was rejected."}}}); }
			}, true, ShaderMap->EncodedInput);
	}
	auto MaterialBuildPrivate::FMaterialCompilation::ProcessTranslation(std::shared_ptr<FMaterialCompilation> ShaderMap, FMaterialTranslatedProgram Translation) -> void
	{
		if (FShaderCompilingManager::PollBuildCancellation(ShaderMap)) return CompleteCompilation(ShaderMap, FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled});
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
		// Cache hits and shared subscribers bind once before completion.
		if (Retained) Retained = MaterialBuildPrivate::BindMaterialProgramCollections(std::move(Retained), ShaderMap->Input->Collections, &ShaderMap->Input->Environment);
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
			// Transfer from the shared producer to this subscriber's own completion task.
			static_cast<DerivedData::IRequestOwner&>(*Subscriber->Owner).End(static_cast<DerivedData::IRequest*>(*ShaderMap->BuildOwner), [&] {
				Subscriber->Owner->LaunchTask("MaterialCompletion", [Subscriber, Program = std::move(Program)] { CompleteCompilation(Subscriber, {}, Program); });
			});
		}
		auto Deliver = [ShaderMap, Build = std::move(Build), Callback = std::move(Callback), Hook = std::move(Hook), Quality]() mutable {
			if (Hook) Hook(Quality, true);
			if (FShaderCompilingManager::PollCancellation(ShaderMap)) { Build.Program = std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled}); Build.RetainedHit = false; }
			Build.Sources = std::move(ShaderMap->Sources);
			Callback(std::move(Build));
			if (Hook) Hook(Quality, false);
		};
		if (ShaderMap->SingleFlight) Deliver();
		else static_cast<DerivedData::IRequestOwner&>(*ShaderMap->Owner).End(static_cast<DerivedData::IRequest*>(*ShaderMap->BuildOwner), [&] {
			ShaderMap->Owner->LaunchTask("MaterialCompletion", std::move(Deliver));
		});
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

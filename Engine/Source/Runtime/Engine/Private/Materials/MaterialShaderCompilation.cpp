#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Threading/Task.h"
#include "MaterialPreparedProgram.h"

#if DURIN_WITH_EDITOR
#include "MaterialTranslation.h"
#include "MaterialTranslationCodec.h"
#include "Shader/IShaderBuildModule.h"
#include "Threading/TaskComposition.h"
#endif

namespace Durin
{
	using namespace MaterialBuildPrivate;
	namespace
	{
		auto CanceledMaterial() -> FMaterialCompilerResult
		{ return {.Status = EMaterialProgramStatus::Canceled}; }
		auto FailedMaterial(std::string Diagnostic = {}) -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile,
			.Error = FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, std::move(Diagnostic))}}}; }
	}


#if DURIN_WITH_EDITOR


	auto FShaderCompilingManager::SubmitJobs(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return Submit(std::move(Input), std::move(Options), std::move(Completion), false); }
	auto FShaderCompilingManager::SubmitJobs(const MIR::FCompilerInput& Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{
		auto Borrowed = std::shared_ptr<const MIR::FCompilerInput>(&Input, [](const auto*) {});
		return Submit(std::move(Borrowed), std::move(Options), std::move(Completion), true);
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
			ShaderMap = Options.ShaderMap ? std::move(Options.ShaderMap) : std::make_shared<FMaterialShaderMap>();
			require(ShaderMap->CompilingId == 0);
			static std::atomic_uint64_t NextCompilingId = 1;
			ShaderMap->CompilingId = NextCompilingId.fetch_add(1);
			ShaderMap->Owner = std::make_shared<DerivedData::FRequestOwner>();
			ShaderMap->Input = CopyInput ? std::make_shared<const MIR::FCompilerInput>(*Input) : std::move(Input);
			ShaderMap->EncodedInput = std::move(Options.EncodedInput);
			ShaderMap->Force = Options.bForceRecompile; ShaderMap->Cancellation = std::move(Options.Cancellation);
			ShaderMap->BuildCancellation = ShaderMap->BuildCancellation.WithObserver([Weak = std::weak_ptr(ShaderMap)] {
				const auto Value = Weak.lock(); return !Value || IsBuildCanceled(Value);
			});
			ShaderMap->Completion = std::move(Completion);
			ShaderMap->Inline = !IsTaskSchedulerRunning();
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		CompilingShaderMaps.push_back(ShaderMap);
		AdmissionLock.unlock();
		auto Handle = ShaderMap;
		LaunchTask(ShaderMap, [ShaderMap] { FMaterialShaderMap::Compile(ShaderMap); });
		return Handle;
	}

	auto FShaderCompilingManager::LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void
	{
		if (ShaderMap->Inline) {
			try { if (IsBuildCanceled(ShaderMap)) FMaterialShaderMap::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
			catch (...) { FMaterialShaderMap::CompleteCompilation(ShaderMap, FailedMaterial()); }
			return;
		}
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		ETaskPriority Priority;
		{ std::lock_guard Lock(ShaderMap->Mutex); Priority = ShaderMap->HighPriority ? ETaskPriority::High : ETaskPriority::Normal; }
		auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("MaterialCompileStage",
			[ShaderMap, Work = std::move(Work)](const FTaskCancellationToken&) {
				try { if (IsBuildCanceled(ShaderMap)) FMaterialShaderMap::CompleteCompilation(ShaderMap, CanceledMaterial()); else Work(); }
				catch (...) { FMaterialShaderMap::CompleteCompilation(ShaderMap, FailedMaterial()); }
			}, [ShaderMap](ETaskState Terminal) {
				if (Terminal == ETaskState::Canceled) FMaterialShaderMap::CompleteCompilation(ShaderMap, CanceledMaterial());
				else if (Terminal != ETaskState::Succeeded) FMaterialShaderMap::CompleteCompilation(ShaderMap, FailedMaterial());
			}, {.CancellationToken = ShaderMap->CpuCancellation.GetToken(), .Priority = Priority});
		if (!Dispatch.HasValue()) FMaterialShaderMap::CompleteCompilation(ShaderMap, FailedMaterial());
		else {
			std::lock_guard Lock(ShaderMap->Mutex);
			auto Task = std::move(Dispatch).TakeValue();
			ShaderMap->Owner->TrackTask(std::move(Task));
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
						.ShaderMap = Cached, .Timings = Cached->CompilationTimings});
			auto& Programs = CompilingPrograms[ShaderMap->Force];
			const auto Entry = Programs.find(ShaderMap->ShaderMapId);
			auto Existing = Entry == Programs.end() ? FShaderMapPtr{} : Entry->second.lock();
			if (Existing && !Existing->BuildCancellation.IsCancellationRequested()) {
				ShaderMap->SingleFlight = true;
				ProgramJoins.fetch_add(1);
				ShaderMap->Owner->TrackDependency({
					.IsFinished = [Owner = Existing->Owner] { return Owner->Poll(); },
					.Wait = [Owner = Existing->Owner] { return Owner->Wait(); },
					.RequestCancel = [] {}});
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
		auto Submitted = Manager.SubmitJobs(Input, {.bForceRecompile = Force},
			[&](FMaterialBuildResult Value) { Result = std::move(Value); });
		if (!Submitted) return {.Program = std::make_shared<const FMaterialCompilerResult>(FailedMaterial())};
		Manager.FinishCompilation(*Submitted);
		return Result;
	}
#else
	auto FShaderCompilingManager::SubmitJobs(std::shared_ptr<const MIR::FCompilerInput>, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return std::unexpected(EMaterialCompileAdmissionError::Unavailable); }
	auto FShaderCompilingManager::SubmitJobs(const MIR::FCompilerInput&, FMaterialProgramCompileOptions,
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
		.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::NoAuthoredProgram}}})}; }
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
}

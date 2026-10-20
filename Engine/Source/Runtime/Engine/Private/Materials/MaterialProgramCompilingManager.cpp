#include "MaterialProgramCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Threading/Task.h"

#if DURIN_WITH_EDITOR
#include "MaterialTranslation.h"
#include "MaterialTranslationCodec.h"
#include "Shader/IShaderBuildModule.h"
#include "Threading/TaskComposition.h"
#endif

namespace Durin::MaterialBuildPrivate
{
	namespace
	{
		auto RaisePeak(std::atomic_uint64_t& Peak, uint64 Value) -> void
		{ auto Previous = Peak.load(); while (Previous < Value && !Peak.compare_exchange_weak(Previous, Value)) {} }
		auto CanceledMaterial() -> FMaterialCompilerResult
		{ return {.Status = EMaterialProgramStatus::Canceled}; }
		auto FailedMaterial(std::string Diagnostic = {}) -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile,
			.Error = FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, std::move(Diagnostic))}}}; }
	}

	auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats
	{ auto& Manager = FMaterialProgramCompilingManager::Get(); return {Manager.ProgramJoins.load(), Manager.PeakActiveShaders.load(), Manager.PeakOutputBytes.load()}; }

#if DURIN_WITH_EDITOR

	FMaterialProgramCompilingManager::FMaterialProgramCompilingManager() = default;
	FMaterialProgramCompilingManager::~FMaterialProgramCompilingManager() = default;
	auto FMaterialProgramCompilingManager::Get() -> FMaterialProgramCompilingManager&
	{ static FMaterialProgramCompilingManager Manager; return Manager; }

	auto FMaterialProgramCompilingManager::SubmitJobs(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return Submit(std::move(Input), std::move(Options), std::move(Completion), false); }
	auto FMaterialProgramCompilingManager::SubmitJobs(const MIR::FCompilerInput& Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{
		auto Borrowed = std::shared_ptr<const MIR::FCompilerInput>(&Input, [](const auto*) {});
		return Submit(std::move(Borrowed), std::move(Options), std::move(Completion), true);
	}
	auto FMaterialProgramCompilingManager::Submit(std::shared_ptr<const MIR::FCompilerInput> Input,
		FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion, bool CopyInput)
		-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{
		ProcessAsyncResults();
		if (!Input || !Completion) return std::unexpected(EMaterialCompileAdmissionError::Unavailable);
		// Admission precedes encoding, normalization or any input/product copy.
		FShaderMapPtr ShaderMap;
		std::unique_lock AdmissionLock(Mutex);
			if (!Accepting) return std::unexpected(EMaterialCompileAdmissionError::Closed);
			if (CompilingShaderMaps.size() >= 32) return std::unexpected(EMaterialCompileAdmissionError::Capacity);
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

	auto FMaterialProgramCompilingManager::LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void
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
			if (!ShaderMap->TaskId) ShaderMap->TaskId = Task.GetTaskId();
			ShaderMap->Owner->TrackTask(std::move(Task));
		}
	}


	auto FMaterialProgramCompilingManager::SubmitCompileJobs(FShaderMapPtr ShaderMap) -> void
	{
		// One driver avoids recursive dispatch when SubmitGenerated completes inline.
		DerivedData::FRequestBarrier DispatchBarrier(*ShaderMap->Owner);
		{ std::lock_guard Lock(ShaderMap->Mutex); if (ShaderMap->ResultReady || ShaderMap->Dispatching || ShaderMap->AssemblyScheduled) return; ShaderMap->Dispatching = true; }
		for (;;) {
			const bool Canceled = IsBuildCanceled(ShaderMap);
			size_t Index;
			EShaderBuildPriority Priority;
			{
				std::lock_guard Lock(ShaderMap->Mutex);
				const bool Failed = Canceled || std::ranges::any_of(ShaderMap->Outputs, [](const auto& Output) { return Output && !*Output; });
				if (!ShaderMap->ActiveJobs && (Failed || ShaderMap->NextJob == ShaderMap->Jobs.size())) {
					ShaderMap->AssemblyScheduled = true; ShaderMap->Dispatching = false;
					break;
				}
				if (Failed || ShaderMap->ActiveJobs == 2 || ShaderMap->NextJob == ShaderMap->Jobs.size()) {
					ShaderMap->Dispatching = false; return;
				}
				Index = ShaderMap->NextJob++; ++ShaderMap->ActiveJobs;
				Priority = ShaderMap->HighPriority ? EShaderBuildPriority::High : EShaderBuildPriority::Normal;
				// Two requests each reserve at most the remaining aggregate allowance.
				// Retained outputs + live output reservations never exceed 16 MiB.
				ShaderMap->Jobs[Index].MaximumOutputBytes = std::min(ShaderMap->Jobs[Index].MaximumOutputBytes,
					MaterialCompileMaxResultBytes - ShaderMap->RetainedStageBytes);
				ShaderMap->ReservedOutputBytes += ShaderMap->Jobs[Index].MaximumOutputBytes;
				RaisePeak(Get().PeakActiveShaders, ShaderMap->ActiveJobs);
				RaisePeak(Get().PeakOutputBytes, ShaderMap->RetainedStageBytes + ShaderMap->ReservedOutputBytes);
			}
			auto* ShaderBuild = IShaderBuildModule::Get();
			if (!ShaderBuild) { ProcessFinishedJob(ShaderMap, Index, FShaderBuildResult::Failure("Shader build module unavailable.")); continue; }
			DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
			auto Submitted = ShaderBuild->SubmitGenerated(std::move(ShaderMap->Jobs[Index]),
				{.Priority = Priority, .IsCancelled = [Weak = std::weak_ptr(ShaderMap)] { const auto Value = Weak.lock(); return !Value || IsBuildCanceled(Value); }, .bInlineExecution = ShaderMap->Inline},
				[ShaderMap, Index](FShaderBuildResult Output) { ProcessFinishedJob(ShaderMap, Index, std::move(Output)); });
			if (!Submitted) { ProcessFinishedJob(ShaderMap, Index, FShaderBuildResult::Failure(FormatShaderBuildAdmissionError(Submitted.error()))); continue; }
			auto Shader = std::make_shared<FShaderBuildHandle>(std::move(*Submitted));
			ShaderMap->Owner->TrackDependency({
				.IsFinished = [Shader] { return Shader->IsComplete(); },
				.Wait = [Shader] { return Shader->Wait() == EShaderBuildWaitResult::Completed ? DerivedData::ERequestWaitResult::Completed : DerivedData::ERequestWaitResult::WouldBlock; },
				.RequestCancel = [Shader] { Shader->RequestCancel(); }});
			{
				std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->Shaders.push_back(Shader);
				if (ShaderMap->HighPriority) Shader->SetPriority(EShaderBuildPriority::High);
			}
			if (IsBuildCanceled(ShaderMap)) Shader->RequestCancel();
		}
		ShaderMap->Translation->Timings.CompilationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - ShaderMap->CompilationBegin).count();
		LaunchTask(ShaderMap, [ShaderMap] {
			FMaterialShaderMap::CompleteCompilation(ShaderMap, ShaderMap->FinalizeCompilation(ShaderMap));
		});
	}

	auto FMaterialProgramCompilingManager::ProcessFinishedJob(FShaderMapPtr ShaderMap, size_t Index, FShaderBuildResult Output) -> void
	{
		ShaderMap->ProcessCompilationResults(Index, std::move(Output));
		SubmitCompileJobs(ShaderMap);
	}

	auto FMaterialProgramCompilingManager::IsBuildCanceled(FShaderMapPtr ShaderMap) -> bool
	{
		std::vector<FCancellationToken> Tokens;
		{
			std::lock_guard Lock(Get().Mutex);
			if (ShaderMap->BuildCancellation.IsCancellationRequested()) return true;
			Tokens.push_back(ShaderMap->Cancellation);
			for (const auto& Subscriber : ShaderMap->CompileDependencies) Tokens.push_back(Subscriber->Cancellation);
		}
		// Observers can be caller code. Evaluate outside the manager lock, then
		// recheck latched flags under lock so a newly joined subscriber also wins.
		for (const auto& Token : Tokens) (void)Token.IsCancelled();
		std::lock_guard Lock(Get().Mutex);
		if (!ShaderMap->Cancellation.IsCancellationRequested()) return false;
		for (const auto& Subscriber : ShaderMap->CompileDependencies)
			if (!Subscriber->Cancellation.IsCancellationRequested()) return false;
		ShaderMap->BuildCancellation.Cancel(); return true;
	}


	auto FMaterialProgramCompilingManager::FindOrAddCompilingShaderMap(FShaderMapPtr ShaderMap)
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		FShaderMapPtr Shared;
		{
			std::lock_guard Lock(Mutex);
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

	auto FMaterialProgramCompilingManager::FinalizeCompilation(FShaderMapPtr ShaderMap,
		std::shared_ptr<const FMaterialCompilerResult> Program, bool RetainedHit,
		std::vector<FShaderMapPtr>& Subscribers, std::function<void(FMaterialBuildResult)>& Completion) -> FMaterialBuildResult
	{
		std::lock_guard StateLock(Mutex);
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
			.SingleFlight = ShaderMap->SingleFlight, .TaskId = ShaderMap->TaskId};
	}

	auto FMaterialProgramCompilingManager::FindCompilingShaderMap(FShaderMapPtr Request) const -> FShaderMapPtr
	{
		std::lock_guard Lock(Mutex);
		const auto Found = CompilingPrograms[Request->Force].find(Request->GetShaderMapId());
		if (Found != CompilingPrograms[Request->Force].end()) {
			auto Producer = Found->second.lock();
			// A canceled producer can be replaced under the same identity. Only a
			// current dependency may affect its replacement's work or priority.
			if (Producer && (Producer == Request || std::ranges::contains(Producer->CompileDependencies, Request))) return Producer;
		}
		return Request;
	}

	auto FMaterialProgramCompilingManager::PrioritizeCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		auto ShaderMap = FindCompilingShaderMap(Handle);
		std::vector<std::shared_ptr<FShaderBuildHandle>> Shaders;
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->HighPriority = true; Shaders = ShaderMap->Shaders; Owner = ShaderMap->Owner; }
		if (Owner) Owner->SetPriority(DerivedData::ERequestPriority::High);
		for (const auto& Shader : Shaders) Shader->SetPriority(EShaderBuildPriority::High);
	}
	auto FMaterialProgramCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		Handle->Cancellation.Cancel();
		auto ShaderMap = FindCompilingShaderMap(Handle);
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(ShaderMap->Mutex); Owner = ShaderMap->Owner; }
		if (Owner && IsBuildCanceled(ShaderMap)) { ShaderMap->CpuCancellation.RequestCancellation(); Owner->RequestCancel(); }
	}
	auto FMaterialProgramCompilingManager::IsCompiling(const FMaterialProgramCompileHandle& Handle) const -> bool
	{
		if (!Handle) return false;
		std::lock_guard Lock(Handle->Mutex);
		return !Handle->ResultReady || (Handle->Owner && !Handle->Owner->Poll());
	}
	auto FMaterialProgramCompilingManager::FinishCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		PrioritizeCompilation(Handle);
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Handle->Mutex); Owner = Handle->Owner; }
		if (Owner) require(Owner->Wait() == DerivedData::ERequestWaitResult::Completed);
		require(!IsCompiling(Handle));
		ProcessAsyncResults();
	}
	auto FMaterialProgramCompilingManager::ProcessAsyncResults() -> void
	{
		std::vector<FShaderMapPtr> Finished;
		{ std::lock_guard Lock(Mutex); Finished = CompilingShaderMaps; }
		std::erase_if(Finished, [this](const auto& ShaderMap) { return IsCompiling(ShaderMap); });
		// Destroy request owners outside the manager lock: destruction propagates cancellation.
		std::vector<std::shared_ptr<DerivedData::FRequestOwner>> RetiredOwners;
		std::lock_guard Lock(Mutex);
		for (const auto& ShaderMap : Finished) {
			std::lock_guard RecordLock(ShaderMap->Mutex);
			ShaderMap->Input.reset(); ShaderMap->EncodedInput.reset(); ShaderMap->Translation.reset();
			ShaderMap->Shaders.clear(); ShaderMap->Shaders.shrink_to_fit();
			ShaderMap->Jobs.clear(); ShaderMap->Jobs.shrink_to_fit();
			ShaderMap->Outputs.clear(); ShaderMap->Outputs.shrink_to_fit();
			RetiredOwners.push_back(std::move(ShaderMap->Owner));
			std::erase(CompilingShaderMaps, ShaderMap);
		}
	}
	auto FMaterialProgramCompilingManager::FinishAllCompilation() -> void
	{
		for (;;) {
			std::vector<FShaderMapPtr> Records;
			{ std::lock_guard Lock(Mutex); Records = CompilingShaderMaps; }
			if (Records.empty()) return;
			for (const auto& ShaderMap : Records) {
				FinishCompilation(ShaderMap);
			}
			// A completion callback may have submitted another compile while we waited.
		}
	}
	auto FMaterialProgramCompilingManager::Shutdown() -> void
	{
		std::vector<FShaderMapPtr> Records;
		{ std::lock_guard Lock(Mutex); Accepting = false; Records = CompilingShaderMaps; }
		for (const auto& ShaderMap : Records) CancelCompilation(ShaderMap);
		FinishAllCompilation();
	}

	auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult
	{
		FMaterialBuildResult Result;
		auto& Manager = FMaterialProgramCompilingManager::Get();
		auto Submitted = Manager.SubmitJobs(Input, {.bForceRecompile = Force},
			[&](FMaterialBuildResult Value) { Result = std::move(Value); });
		if (!Submitted) return {.Program = std::make_shared<const FMaterialCompilerResult>(FailedMaterial())};
		Manager.FinishCompilation(*Submitted);
		return Result;
	}
#else
	FMaterialProgramCompilingManager::FMaterialProgramCompilingManager() = default;
	FMaterialProgramCompilingManager::~FMaterialProgramCompilingManager() = default;
	auto FMaterialProgramCompilingManager::Get() -> FMaterialProgramCompilingManager&
	{ static FMaterialProgramCompilingManager Manager; return Manager; }
	auto FMaterialProgramCompilingManager::SubmitJobs(std::shared_ptr<const MIR::FCompilerInput>, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return std::unexpected(EMaterialCompileAdmissionError::Unavailable); }
	auto FMaterialProgramCompilingManager::SubmitJobs(const MIR::FCompilerInput&, FMaterialProgramCompileOptions,
		std::function<void(FMaterialBuildResult)>) -> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>
	{ return std::unexpected(EMaterialCompileAdmissionError::Unavailable); }
	auto FMaterialProgramCompilingManager::FinishCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FMaterialProgramCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FMaterialProgramCompilingManager::PrioritizeCompilation(const FMaterialProgramCompileHandle&) -> void {}
	auto FMaterialProgramCompilingManager::IsCompiling(const FMaterialProgramCompileHandle&) const -> bool { return false; }
	auto FMaterialProgramCompilingManager::ProcessAsyncResults() -> void {}
	auto FMaterialProgramCompilingManager::FinishAllCompilation() -> void {}
	auto FMaterialProgramCompilingManager::Shutdown() -> void {}
	auto CompileMaterialInput(const MIR::FCompilerInput&, bool) -> FMaterialBuildResult
	{ return {.Program = std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{
		.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::NoAuthoredProgram}}})}; }
#endif
}

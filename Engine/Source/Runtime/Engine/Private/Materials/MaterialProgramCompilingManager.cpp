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
		std::mutex CompletionHookMutex;
		std::function<void(EMaterialQualityLevel, bool)> CompletionHook;
		std::atomic_uint64_t ProgramJoins = 0, PeakActiveShaders = 0, PeakOutputBytes = 0;
		auto RaisePeak(std::atomic_uint64_t& Peak, uint64 Value) -> void
		{ auto Previous = Peak.load(); while (Previous < Value && !Peak.compare_exchange_weak(Previous, Value)) {} }
		auto CanceledMaterial() -> FMaterialCompilerResult
		{ return {.Status = EMaterialProgramStatus::Canceled}; }
		auto FailedMaterial(std::string Diagnostic = {}) -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile,
			.Error = FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, std::move(Diagnostic))}}}; }
	}
	auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void
	{ std::lock_guard Lock(CompletionHookMutex); CompletionHook = std::move(Hook); }
	auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats
	{ return {ProgramJoins.load(), PeakActiveShaders.load(), PeakOutputBytes.load()}; }

#if DURIN_WITH_EDITOR

	struct FMaterialProgramCompilingManager::FState
	{
		std::mutex Mutex;
		std::vector<FRecord> Records;
		// Protected by this mutex, including subscriber links and removal at completion.
		std::unordered_map<FMaterialProgramIdentity, std::weak_ptr<FMaterialShaderMap>> Programs[2];
		bool Accepting = true;
	};

	FMaterialProgramCompilingManager::FMaterialProgramCompilingManager() : State(std::make_unique<FState>()) {}
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
		FRecord Record;
		std::unique_lock AdmissionLock(State->Mutex);
			if (!State->Accepting) return std::unexpected(EMaterialCompileAdmissionError::Closed);
			if (State->Records.size() >= 32) return std::unexpected(EMaterialCompileAdmissionError::Capacity);
			if (EstimateTranslationCaptureBytes(*Input) > TranslationInputBytes)
				return std::unexpected(EMaterialCompileAdmissionError::InputTooLarge);
			Record = Options.ShaderMap ? std::move(Options.ShaderMap) : std::make_shared<FMaterialShaderMap>();
			require(Record->CompilingId == 0);
			static std::atomic_uint64_t NextCompilingId = 1;
			Record->CompilingId = NextCompilingId.fetch_add(1);
			Record->Owner = std::make_shared<DerivedData::FRequestOwner>();
			Record->Input = CopyInput ? std::make_shared<const MIR::FCompilerInput>(*Input) : std::move(Input);
			Record->EncodedInput = std::move(Options.EncodedInput);
			Record->Force = Options.bForceRecompile; Record->Cancellation = std::move(Options.Cancellation);
			Record->BuildCancellation = Record->BuildCancellation.WithObserver([Weak = std::weak_ptr(Record)] {
				const auto Value = Weak.lock(); return !Value || IsBuildCanceled(Value);
			});
			Record->Completion = std::move(Completion);
			Record->Inline = !IsTaskSchedulerRunning();
		DerivedData::FRequestBarrier Submission(*Record->Owner);
		State->Records.push_back(Record);
		AdmissionLock.unlock();
		auto Handle = Record;
		LaunchTask(Record, [Record] { FMaterialShaderMap::Compile(Record); });
		return Handle;
	}

	auto FMaterialProgramCompilingManager::LaunchTask(FRecord Record, std::function<void()> Work) -> void
	{
		if (Record->Inline) {
			try { if (IsBuildCanceled(Record)) CompleteCompilation(Record, CanceledMaterial()); else Work(); }
			catch (...) { CompleteCompilation(Record, FailedMaterial()); }
			return;
		}
		DerivedData::FRequestBarrier Submission(*Record->Owner);
		ETaskPriority Priority;
		{ std::lock_guard Lock(Record->Mutex); Priority = Record->HighPriority ? ETaskPriority::High : ETaskPriority::Normal; }
		auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("MaterialCompileStage",
			[Record, Work = std::move(Work)](const FTaskCancellationToken&) {
				try { if (IsBuildCanceled(Record)) CompleteCompilation(Record, CanceledMaterial()); else Work(); }
				catch (...) { CompleteCompilation(Record, FailedMaterial()); }
			}, [Record](ETaskState Terminal) {
				if (Terminal == ETaskState::Canceled) CompleteCompilation(Record, CanceledMaterial());
				else if (Terminal != ETaskState::Succeeded) CompleteCompilation(Record, FailedMaterial());
			}, {.CancellationToken = Record->CpuCancellation.GetToken(), .Priority = Priority});
		if (!Dispatch.HasValue()) CompleteCompilation(Record, FailedMaterial());
		else {
			std::lock_guard Lock(Record->Mutex);
			auto Task = std::move(Dispatch).TakeValue();
			if (!Record->TaskId) Record->TaskId = Task.GetTaskId();
			Record->Owner->TrackTask(std::move(Task));
		}
	}

	auto FMaterialProgramCompilingManager::ProcessTranslation(FRecord Record, FMaterialTranslatedProgram Translation) -> void
	{
		if (IsBuildCanceled(Record)) return CompleteCompilation(Record, CanceledMaterial());
		std::shared_ptr<const FMaterialCompilerResult> Retained;
		for (const auto& Location : Translation.Sources) { Record->Sources.push_back(Location.Source); Record->SourceOrdinals.push_back(Location.Ordinal); }
		if (!Translation) return CompleteCompilation(Record, FMaterialShaderMap::FinalizeContent(std::move(Translation), {}, Record));
		Record->ShaderMapId = Translation.Identity;
		FRecord Shared;
		{
			auto& Manager = Get();
			std::lock_guard Lock(Manager.State->Mutex);
			// Cache lookup and joining observe the same handoff as completion.
			if (!Record->Force) Retained = FMaterialShaderMap::Configure(FMaterialShaderMap::FindId(Translation.Identity), *Record->Input);
			auto& Programs = Manager.State->Programs[Record->Force];
			const auto Entry = Programs.find(Translation.Identity);
			auto Existing = Entry == Programs.end() ? FRecord{} : Entry->second.lock();
			if (!Retained && Existing && !Existing->BuildCancellation.IsCancellationRequested()) {
				Record->SingleFlight = true;
				ProgramJoins.fetch_add(1);
				Record->SharedCompile = Existing;
				Record->Owner->TrackDependency({
					.IsFinished = [Owner = Existing->Owner] { return Owner->Poll(); },
					.Wait = [Owner = Existing->Owner] { return Owner->Wait(); },
					.RequestCancel = [] {}});
				Existing->CompileDependencies.push_back(Record);
				Shared = std::move(Existing);
			}
			else if (!Retained) {
				Programs[Translation.Identity] = Record;
				Record->Translation = std::make_unique<FMaterialTranslatedProgram>(std::move(Translation));
			}
		}
		if (Retained) return CompleteCompilation(Record, {}, std::move(Retained));
		if (Shared) {
			bool HighPriority;
			{ std::lock_guard Lock(Record->Mutex); HighPriority = Record->HighPriority; }
			if (HighPriority) { FMaterialProgramCompileHandle Handle; Handle = std::move(Shared); Get().PrioritizeCompilation(Handle); }
			return;
		}
		Record->CompilationBegin = std::chrono::steady_clock::now();
		Record->Jobs = MakeMaterialShaderStageRequests(*Record->Translation, Record->Force);
		Record->Outputs.resize(Record->Jobs.size());
		if (Record->Jobs.empty()) return CompleteCompilation(Record, FailedMaterial("Material has no compilable declared uses."));
		SubmitCompileJobs(Record);
	}

	auto FMaterialProgramCompilingManager::SubmitCompileJobs(FRecord Record) -> void
	{
		// One driver avoids recursive dispatch when SubmitGenerated completes inline.
		DerivedData::FRequestBarrier DispatchBarrier(*Record->Owner);
		{ std::lock_guard Lock(Record->Mutex); if (Record->ResultReady || Record->Dispatching || Record->AssemblyScheduled) return; Record->Dispatching = true; }
		for (;;) {
			const bool Canceled = IsBuildCanceled(Record);
			size_t Index;
			EShaderBuildPriority Priority;
			{
				std::lock_guard Lock(Record->Mutex);
				const bool Failed = Canceled || std::ranges::any_of(Record->Outputs, [](const auto& Output) { return Output && !*Output; });
				if (!Record->ActiveJobs && (Failed || Record->NextJob == Record->Jobs.size())) {
					Record->AssemblyScheduled = true; Record->Dispatching = false;
					break;
				}
				if (Failed || Record->ActiveJobs == 2 || Record->NextJob == Record->Jobs.size()) {
					Record->Dispatching = false; return;
				}
				Index = Record->NextJob++; ++Record->ActiveJobs;
				Priority = Record->HighPriority ? EShaderBuildPriority::High : EShaderBuildPriority::Normal;
				// Two requests each reserve at most the remaining aggregate allowance.
				// Retained outputs + live output reservations never exceed 16 MiB.
				Record->Jobs[Index].MaximumOutputBytes = std::min(Record->Jobs[Index].MaximumOutputBytes,
					MaterialCompileMaxResultBytes - Record->RetainedStageBytes);
				Record->ReservedOutputBytes += Record->Jobs[Index].MaximumOutputBytes;
				RaisePeak(PeakActiveShaders, Record->ActiveJobs);
				RaisePeak(PeakOutputBytes, Record->RetainedStageBytes + Record->ReservedOutputBytes);
			}
			auto* ShaderBuild = IShaderBuildModule::Get();
			if (!ShaderBuild) { ProcessFinishedJob(Record, Index, FShaderBuildResult::Failure("Shader build module unavailable.")); continue; }
			DerivedData::FRequestBarrier Submission(*Record->Owner);
			auto Submitted = ShaderBuild->SubmitGenerated(std::move(Record->Jobs[Index]),
				{.Priority = Priority, .IsCancelled = [Weak = std::weak_ptr(Record)] { const auto Value = Weak.lock(); return !Value || IsBuildCanceled(Value); }, .bInlineExecution = Record->Inline},
				[Record, Index](FShaderBuildResult Output) { ProcessFinishedJob(Record, Index, std::move(Output)); });
			if (!Submitted) { ProcessFinishedJob(Record, Index, FShaderBuildResult::Failure(FormatShaderBuildAdmissionError(Submitted.error()))); continue; }
			auto Shader = std::make_shared<FShaderBuildHandle>(std::move(*Submitted));
			Record->Owner->TrackDependency({
				.IsFinished = [Shader] { return Shader->IsComplete(); },
				.Wait = [Shader] { return Shader->Wait() == EShaderBuildWaitResult::Completed ? DerivedData::ERequestWaitResult::Completed : DerivedData::ERequestWaitResult::WouldBlock; },
				.RequestCancel = [Shader] { Shader->RequestCancel(); }});
			{
				std::lock_guard Lock(Record->Mutex); Record->Shaders.push_back(Shader);
				if (Record->HighPriority) Shader->SetPriority(EShaderBuildPriority::High);
			}
			if (IsBuildCanceled(Record)) Shader->RequestCancel();
		}
		Record->Translation->Timings.CompilationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - Record->CompilationBegin).count();
		LaunchTask(Record, [Record] {
			CompleteCompilation(Record, Record->FinalizeCompilation(Record));
		});
	}

	auto FMaterialProgramCompilingManager::ProcessFinishedJob(FRecord Record, size_t Index, FShaderBuildResult Output) -> void
	{
		Record->ProcessCompilationResults(Index, std::move(Output));
		SubmitCompileJobs(Record);
	}

	auto FMaterialProgramCompilingManager::IsBuildCanceled(FRecord Record) -> bool
	{
		std::vector<FCancellationToken> Tokens;
		{
			std::lock_guard Lock(Get().State->Mutex);
			if (Record->BuildCancellation.IsCancellationRequested()) return true;
			Tokens.push_back(Record->Cancellation);
			for (const auto& Subscriber : Record->CompileDependencies) Tokens.push_back(Subscriber->Cancellation);
		}
		// Observers can be caller code. Evaluate outside the manager lock, then
		// recheck latched flags under lock so a newly joined subscriber also wins.
		for (const auto& Token : Tokens) (void)Token.IsCancelled();
		std::lock_guard Lock(Get().State->Mutex);
		if (!Record->Cancellation.IsCancellationRequested()) return false;
		for (const auto& Subscriber : Record->CompileDependencies)
			if (!Subscriber->Cancellation.IsCancellationRequested()) return false;
		Record->BuildCancellation.Cancel(); return true;
	}

	auto FMaterialProgramCompilingManager::CompleteCompilation(FRecord Record, FMaterialCompilerResult Result,
		std::shared_ptr<const FMaterialCompilerResult> Retained) -> void
	{
		DerivedData::FRequestBarrier DeliveryBarrier(*Record->Owner);
		std::function<void(FMaterialBuildResult)> Callback;
		std::vector<FRecord> Subscribers;
		uint64 TaskId;
		if (Result && FMaterialShaderMap::GetRetainedBytes(Result) > MaterialCompileMaxResultBytes)
			Result = {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
		const auto Quality = Retained ? Retained->Quality : Result.Quality;
		FMaterialBuildResult Build;
		{
			std::lock_guard StateLock(Get().State->Mutex);
			std::lock_guard Lock(Record->Mutex); if (Record->ResultReady) return; Record->ResultReady = true;
			Callback = std::move(Record->Completion); TaskId = Record->TaskId;
			Subscribers = std::move(Record->CompileDependencies);
			Build.RetainedHit = bool(Retained) && !Record->SingleFlight; Build.SingleFlight = Record->SingleFlight; Build.TaskId = TaskId;
			Build.Program = Retained ? std::move(Retained) : std::make_shared<const FMaterialCompilerResult>(std::move(Result));
			// Publish before removing the in-flight identity, under the lookup lock.
			if (*Build.Program && !Build.RetainedHit && !Record->SingleFlight) Build.Program = FMaterialShaderMap::Register(std::move(Build.Program), Record->Force);
			auto& Programs = Get().State->Programs[Record->Force];
			const auto Existing = Programs.find(Record->ShaderMapId);
			if (Existing != Programs.end() && Existing->second.lock() == Record) Programs.erase(Existing);
		}
		if (!Callback) return;
		std::function<void(EMaterialQualityLevel, bool)> Hook;
		{ std::lock_guard Lock(CompletionHookMutex); Hook = CompletionHook; }
		for (const auto& Subscriber : Subscribers) {
			auto Program = FMaterialShaderMap::Configure(Build.Program, *Subscriber->Input);
			if (Program && !*Program) {
				auto Qualified = std::make_shared<FMaterialCompilerResult>(*Program);
				for (auto& Diagnostic : Qualified->Diagnostics) {
					const auto Origin = std::ranges::find(Record->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
					if (Origin == Record->Sources.end()) continue;
					const auto Location = std::ranges::find(Subscriber->Sources, Origin->ExpressionIndex, &MIR::FSource::ExpressionIndex);
					if (Location == Subscriber->Sources.end()) continue;
					const auto& Source = *Location;
					Diagnostic.NodeId = Source.NodeId; Diagnostic.PortId = Source.PortId;
					Diagnostic.FunctionAssetPath = Source.FunctionAssetPath; Diagnostic.CallPath = Source.CallPath;
				}
				Program = std::move(Qualified);
			}
			// A subscriber callback cannot prevent another subscriber from retiring.
			try { LaunchTask(Subscriber, [Subscriber, Program = std::move(Program)] { CompleteCompilation(Subscriber, {}, Program); }); } catch (...) {}
		}
		if (Hook) Hook(Quality, true);
		if (Record->Cancellation.IsCancelled()) { Build.Program = std::make_shared<const FMaterialCompilerResult>(CanceledMaterial()); Build.RetainedHit = false; }
		else Build.Program = FMaterialShaderMap::Configure(std::move(Build.Program), *Record->Input);
		Build.Sources = std::move(Record->Sources);
		Build.SourceOrdinals = std::move(Record->SourceOrdinals);
		Callback(std::move(Build));
		if (Hook) Hook(Quality, false);
	}

	auto FMaterialProgramCompilingManager::PrioritizeCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		auto Record = Handle;
		{ std::lock_guard Lock(State->Mutex); if (auto Shared = Record->SharedCompile.lock()) Record = std::move(Shared); }
		std::vector<std::shared_ptr<FShaderBuildHandle>> Shaders;
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Record->Mutex); Record->HighPriority = true; Shaders = Record->Shaders; Owner = Record->Owner; }
		if (Owner) Owner->SetPriority(DerivedData::ERequestPriority::High);
		for (const auto& Shader : Shaders) Shader->SetPriority(EShaderBuildPriority::High);
	}
	auto FMaterialProgramCompilingManager::CancelCompilation(const FMaterialProgramCompileHandle& Handle) -> void
	{
		if (!Handle) return;
		auto Record = Handle;
		Record->Cancellation.Cancel();
		{ std::lock_guard Lock(State->Mutex); if (auto Shared = Record->SharedCompile.lock()) Record = std::move(Shared); }
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		{ std::lock_guard Lock(Record->Mutex); Owner = Record->Owner; }
		if (Owner && IsBuildCanceled(Record)) { Record->CpuCancellation.RequestCancellation(); Owner->RequestCancel(); }
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
		std::vector<FRecord> Finished;
		{ std::lock_guard Lock(State->Mutex); Finished = State->Records; }
		std::erase_if(Finished, [this](const auto& Record) { return IsCompiling(Record); });
		// Destroy request owners outside the manager lock: destruction propagates cancellation.
		std::vector<std::shared_ptr<DerivedData::FRequestOwner>> RetiredOwners;
		std::lock_guard Lock(State->Mutex);
		for (const auto& Record : Finished) {
			std::lock_guard RecordLock(Record->Mutex);
			Record->Input.reset(); Record->EncodedInput.reset(); Record->Translation.reset();
			Record->Shaders.clear(); Record->Shaders.shrink_to_fit();
			Record->Jobs.clear(); Record->Jobs.shrink_to_fit();
			Record->Outputs.clear(); Record->Outputs.shrink_to_fit();
			RetiredOwners.push_back(std::move(Record->Owner));
			std::erase(State->Records, Record);
		}
	}
	auto FMaterialProgramCompilingManager::FinishAllCompilation() -> void
	{
		for (;;) {
			std::vector<FRecord> Records;
			{ std::lock_guard Lock(State->Mutex); Records = State->Records; }
			if (Records.empty()) return;
			for (const auto& Record : Records) {
				FinishCompilation(Record);
			}
			// A completion callback may have submitted another compile while we waited.
		}
	}
	auto FMaterialProgramCompilingManager::Shutdown() -> void
	{
		std::vector<FRecord> Records;
		{ std::lock_guard Lock(State->Mutex); State->Accepting = false; Records = State->Records; }
		for (const auto& Record : Records) CancelCompilation(Record);
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
	struct FMaterialProgramCompilingManager::FState {};
	FMaterialProgramCompilingManager::FMaterialProgramCompilingManager() : State(std::make_unique<FState>()) {}
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

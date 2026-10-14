#include "MaterialBuildOperation.h"
#include "MaterialProgramCache.h"
#if DURIN_WITH_EDITOR
#include "MaterialTranslation.h"
#include "MaterialTranslationCodec.h"
#include "Shader/IShaderBuildModule.h"
#include "Threading/TaskComposition.h"

namespace Durin::MaterialBuildPrivate
{
	namespace
	{
		std::atomic_uint32_t ReservedOperations = 0;
		// Conservative per-operation reservations: 16 MiB active products plus
		// 8 MiB assembly overlap. Admission precedes transformation allocation.
		constexpr uint32 MaximumReservedOperations = 32;
		auto CanceledMaterial() -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::CompilationCanceled}}}; }
		auto FailedMaterial() -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}}}; }
	}
	struct FMaterialBuildOperation::FState
	{
		mutable std::mutex Mutex;
		std::condition_variable Condition;
		std::vector<FTaskHandle> Tasks;
		std::shared_ptr<FShaderBuildHandle> Shader;
		DerivedData::FRequestOwner TranslationOwner;
		DerivedData::FBuildCancellation Cancellation;
		FTaskCancellationToken Parent;
		FTaskCancellationSource CpuCancellation;
		std::shared_ptr<const MIR::FCompilerInput> Input;
		std::function<bool(FMaterialTranslatedProgram&)> OnTranslation;
		std::function<void(FMaterialCompilerResult)> Completion;
		bool Force = false, Started = false, Done = false, Reserved = false;
		bool HighPriority = false;
		uint32 Submitting = 0;
		uint64 TaskId = 0;
	};
	FMaterialBuildOperation::FMaterialBuildOperation() : State(std::make_unique<FState>()) {}
	FMaterialBuildOperation::~FMaterialBuildOperation() { if (State->Reserved) ReservedOperations.fetch_sub(1); }
	auto FMaterialBuildOperation::TryReserve() -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->Reserved) return true;
		auto Count = ReservedOperations.load();
		while (Count < MaximumReservedOperations) if (ReservedOperations.compare_exchange_weak(Count, Count + 1)) { State->Reserved = true; return true; }
		return false;
	}
	auto FMaterialBuildOperation::RaisePriority() -> void
	{
		std::shared_ptr<FShaderBuildHandle> Shader;
		{ std::lock_guard Lock(State->Mutex); State->HighPriority = true; Shader = State->Shader; }
		State->TranslationOwner.SetPriority(DerivedData::ERequestPriority::High);
		if (Shader) Shader->SetPriority(EShaderBuildPriority::High);
	}
	auto FMaterialBuildOperation::Launch(std::function<void()> Work) -> void
	{
		ETaskPriority Priority;
		{ std::lock_guard Lock(State->Mutex); ++State->Submitting; Priority = State->HighPriority ? ETaskPriority::High : ETaskPriority::Normal; }
		auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("MaterialBuildStage",
			[Self = shared_from_this(), Work = std::move(Work)](const FTaskCancellationToken&) {
				try { if (Self->State->Cancellation.IsCancelled()) Self->Complete(CanceledMaterial()); else Work(); }
				catch (...) { Self->Complete(FailedMaterial()); }
			}, [Self = shared_from_this()](ETaskState Terminal) { if (Terminal != ETaskState::Succeeded) Self->Complete(CanceledMaterial()); }, {.CancellationToken = State->CpuCancellation.GetToken(), .Priority = Priority});
		if (!Dispatch.HasValue()) Complete(FailedMaterial());
		{
			std::lock_guard Lock(State->Mutex);
			if (Dispatch.HasValue()) { auto Task = std::move(Dispatch).TakeValue(); if (!State->TaskId) State->TaskId = Task.GetTaskId(); State->Tasks.push_back(std::move(Task)); }
			--State->Submitting;
		}
		State->Condition.notify_all();
	}
	auto FMaterialBuildOperation::Start(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force,
		FTaskCancellationToken Cancellation, std::function<bool(FMaterialTranslatedProgram&)> OnTranslation,
		std::function<void(FMaterialCompilerResult)> Completion) -> void
	{
		const bool Admitted = TryReserve();
		{
			std::lock_guard Lock(State->Mutex); require(!State->Started); State->Started = true;
			State->Input = std::move(Input); State->Force = Force; State->Parent = std::move(Cancellation);
			State->Cancellation = State->Cancellation.WithObserver([Parent = State->Parent] { return Parent.IsCancellationRequested(); });
			State->OnTranslation = std::move(OnTranslation); State->Completion = std::move(Completion);
		}
		if (!Admitted) return Complete(FailedMaterial());
		Launch([Self = shared_from_this()] {
			if (auto Error = ValidateTranslationConsumer(*Self->State->Input); Error.HasError())
				return Self->Complete({.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Normalization, .Error = std::move(Error)}}});
			auto Admitted = SubmitTranslation(Self->State->Input, Self->State->Force, Self->State->TranslationOwner,
				[Self](FMaterialTranslatedProgram Product) { try { Self->Translated(std::move(Product)); } catch (...) { Self->Complete(FailedMaterial()); } }, Self->State->Cancellation);
			if (!Admitted) Self->Complete(FailedMaterial());
		});
	}
	auto FMaterialBuildOperation::Translated(FMaterialTranslatedProgram Translation) -> void
	{
		if (State->Cancellation.IsCancelled()) return Complete(CanceledMaterial());
		std::function<bool(FMaterialTranslatedProgram&)> Join;
		{ std::lock_guard Lock(State->Mutex); Join = std::move(State->OnTranslation); }
		if (Join && !Join(Translation)) return Complete({}, false);
		if (!Translation) return Complete(std::move(Translation.Candidate));
		auto* ShaderBuild = IShaderBuildModule::Get();
		if (!ShaderBuild) return Complete(FailedMaterial());
		auto Product = std::make_shared<FMaterialTranslatedProgram>(std::move(Translation));
		const auto Begin = std::chrono::steady_clock::now();
		EShaderBuildPriority Priority;
		{ std::lock_guard Lock(State->Mutex); ++State->Submitting; Priority = State->HighPriority ? EShaderBuildPriority::High : EShaderBuildPriority::Normal; }
		auto Submitted = ShaderBuild->SubmitGenerated(MakeMaterialShaderCompileRequest(*Product, State->Force),
			{.Priority = Priority, .IsCancelled = [Cancellation = State->Cancellation] { return Cancellation.IsCancelled(); }},
			[Self = shared_from_this(), Product, Begin](FShaderCompilerOutput Output) {
				Product->Candidate.Timings.CompilationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count();
				Self->Launch([Self, Product, Output = std::move(Output)]() mutable {
					if (Self->State->Cancellation.IsCancelled()) return Self->Complete(CanceledMaterial());
					Self->Complete(AssembleMaterialProgram(std::move(*Product), std::move(Output)));
				});
			});
		std::shared_ptr<FShaderBuildHandle> Shader;
		if (Submitted) Shader = std::make_shared<FShaderBuildHandle>(std::move(*Submitted));
		else Complete(FailedMaterial());
		{
			std::lock_guard Lock(State->Mutex); State->Shader = Shader; if (Shader && State->HighPriority) Shader->SetPriority(EShaderBuildPriority::High); --State->Submitting;
		}
		State->Condition.notify_all();
		if (State->Cancellation.IsCancelled() && Shader) Shader->RequestCancel();
	}
	auto FMaterialBuildOperation::Complete(FMaterialCompilerResult Result, bool Deliver) -> void
	{
		std::function<void(FMaterialCompilerResult)> Callback;
		{
			std::lock_guard Lock(State->Mutex); if (State->Done) return; State->Done = true;
			Callback = std::move(State->Completion); State->OnTranslation = {};
		}
		if (Result && EstimateProgramBytes(Result) > 8ull * 1024 * 1024) Result = {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds, .Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
		if (State->Cancellation.IsCancelled()) Result = CanceledMaterial();
		if (Deliver && Callback) Callback(std::move(Result));
		State->Condition.notify_all();
	}
	auto FMaterialBuildOperation::Cancel() -> void
	{
		State->Cancellation.Cancel(); State->CpuCancellation.RequestCancellation(); State->TranslationOwner.RequestCancel();
		std::shared_ptr<FShaderBuildHandle> Shader;
		{ std::lock_guard Lock(State->Mutex); Shader = State->Shader; }
		if (Shader) Shader->RequestCancel();
	}
	auto FMaterialBuildOperation::IsRetired() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return State->Started && State->Done && !State->Submitting && State->TranslationOwner.Poll()
			&& (!State->Shader || State->Shader->IsComplete())
			&& std::ranges::all_of(State->Tasks, [](const auto& Task) { return Task.IsComplete(); });
	}
	auto FMaterialBuildOperation::GetTaskId() const -> uint64 { std::lock_guard Lock(State->Mutex); return State->TaskId; }
	auto FMaterialBuildOperation::Wait() -> void
	{
		RaisePriority();
		for (;;)
		{
			std::vector<FTaskHandle> Tasks;
			{ std::unique_lock Lock(State->Mutex); State->Condition.wait(Lock, [&] { return !State->Submitting; }); Tasks = State->Tasks; }
			for (const auto& Task : Tasks) require(WaitTask(Task).WaitStatus == ETaskWaitStatus::Completed);
			require(State->TranslationOwner.Wait() == DerivedData::ERequestWaitResult::Completed);
			std::shared_ptr<FShaderBuildHandle> Shader;
			{ std::unique_lock Lock(State->Mutex); State->Condition.wait(Lock, [&] { return !State->Submitting; }); Shader = State->Shader; }
			if (Shader) require(Shader->Wait() == EShaderBuildWaitResult::Completed);
			if (IsRetired()) return;
		}
	}
	auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force, std::vector<MIR::FSource>* Sources,
		std::shared_ptr<const FMaterialCompilerResult>* Shared, bool* RetainedHit) -> FMaterialCompilerResult
	{
		if (EstimateTranslationCaptureBytes(Input) > TranslationInputBytes) return FailedMaterial();
		auto Operation = std::make_shared<FMaterialBuildOperation>();
		if (!Operation->TryReserve()) return FailedMaterial();
		if (!IsTaskSchedulerRunning())
		{
			auto Product = TranslateMaterialInput(Input, Force);
			if (Sources) *Sources = Product.Sources;
			if (!Product) return std::move(Product.Candidate);
			if (!Force) if (auto Retained = ConfigureProgram(FindProgram(Product.Candidate.Identity), Input)) { if (Shared) *Shared = Retained; if (RetainedHit) *RetainedHit = true; return *Retained; }
			const auto Begin = std::chrono::steady_clock::now();
			auto Output = CompileGeneratedShader(MakeMaterialShaderCompileRequest(Product, Force));
			Product.Candidate.Timings.CompilationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count();
			auto Result = AssembleMaterialProgram(std::move(Product), std::move(Output));
			if (Result && EstimateProgramBytes(Result) > 8ull * 1024 * 1024) return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds, .Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
			if (Result) { auto Program = RetainProgram(std::make_shared<const FMaterialCompilerResult>(Result), Force); if (Shared) *Shared = std::move(Program); }
			return Result;
		}
		FMaterialCompilerResult Result;
		Operation->Start(std::make_shared<const MIR::FCompilerInput>(Input), Force, {},
			[Sources, Shared, RetainedHit, Force, &Input, &Result](auto& Product) {
				if (Sources) *Sources = Product.Sources;
				if (Product && !Force) if (auto Retained = ConfigureProgram(FindProgram(Product.Candidate.Identity), Input)) { Result = *Retained; if (Shared) *Shared = Retained; if (RetainedHit) *RetainedHit = true; return false; }
				return true;
			}, [&](auto Value) { Result = std::move(Value); });
		Operation->Wait();
		if (Result && (!Shared || !*Shared)) { auto Program = RetainProgram(std::make_shared<const FMaterialCompilerResult>(Result), Force); if (Shared) *Shared = std::move(Program); }
		return Result;
	}
}
#else
namespace Durin::MaterialBuildPrivate
{
	struct FMaterialBuildOperation::FState {};
	FMaterialBuildOperation::FMaterialBuildOperation() : State(std::make_unique<FState>()) {}
	FMaterialBuildOperation::~FMaterialBuildOperation() = default;
	auto FMaterialBuildOperation::Start(std::shared_ptr<const MIR::FCompilerInput>, bool, FTaskCancellationToken,
		std::function<bool(FMaterialTranslatedProgram&)>, std::function<void(FMaterialCompilerResult)> Completion) -> void
	{ Completion(CompileMaterialInput({}, false)); }
	auto FMaterialBuildOperation::TryReserve() -> bool { return false; }
	auto FMaterialBuildOperation::RaisePriority() -> void {}
	auto FMaterialBuildOperation::Cancel() -> void {}
	auto FMaterialBuildOperation::Wait() -> void {}
	auto FMaterialBuildOperation::IsRetired() const -> bool { return true; }
	auto FMaterialBuildOperation::GetTaskId() const -> uint64 { return 0; }
	auto CompileMaterialInput(const MIR::FCompilerInput&, bool, std::vector<MIR::FSource>*, std::shared_ptr<const FMaterialCompilerResult>*, bool*) -> FMaterialCompilerResult
	{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::NoAuthoredProgram}}}; }
}
#endif

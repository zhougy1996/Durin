#include "MaterialBuildOperation.h"
#include "MaterialProgramCache.h"
#include "Materials/MaterialCompileLifecycle.h"
namespace Durin::MaterialBuildPrivate
{
	namespace {
		std::mutex CompletionHookMutex;
		std::function<void(EMaterialQualityLevel, bool)> CompletionHook;
	}
	auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void
	{ std::lock_guard Lock(CompletionHookMutex); CompletionHook = std::move(Hook); }
}
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
		{ return {.Status = EMaterialProgramStatus::Canceled}; }
		auto FailedMaterial(std::string Diagnostic = {}) -> FMaterialCompilerResult
		{ return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, std::move(Diagnostic))}}}; }
	}
	struct FMaterialBuildOperation::FState
	{
		mutable std::mutex Mutex;
		std::shared_ptr<FShaderBuildHandle> Shader;
		DerivedData::FRequestOwner Owner;
		DerivedData::FBuildCancellation Cancellation;
		FTaskCancellationSource CpuCancellation;
		std::shared_ptr<const MIR::FCompilerInput> Input;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		std::function<EMaterialTranslationAction(FMaterialTranslatedProgram&)> OnTranslation;
		std::function<void(FMaterialCompilerResult)> Completion;
		bool Force = false, Started = false, ResultReady = false, Reserved = false;
		bool HighPriority = false;
		bool Inline = false;
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
		State->Owner.SetPriority(DerivedData::ERequestPriority::High);
		if (Shader) Shader->SetPriority(EShaderBuildPriority::High);
	}
	auto FMaterialBuildOperation::Launch(std::function<void()> Work) -> void
	{
		if (State->Inline)
		{
			try { if (State->Cancellation.IsCancelled()) Complete(CanceledMaterial()); else Work(); }
			catch (...) { Complete(FailedMaterial()); }
			return;
		}
		DerivedData::FRequestBarrier Submission(State->Owner);
		ETaskPriority Priority;
		{ std::lock_guard Lock(State->Mutex); Priority = State->HighPriority ? ETaskPriority::High : ETaskPriority::Normal; }
		auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("MaterialBuildStage",
			[Self = shared_from_this(), Work = std::move(Work)](const FTaskCancellationToken&) {
				try { if (Self->State->Cancellation.IsCancelled()) Self->Complete(CanceledMaterial()); else Work(); }
				catch (...) { Self->Complete(FailedMaterial()); }
			}, [Self = shared_from_this()](ETaskState Terminal) { if (Terminal == ETaskState::Canceled) Self->Complete(CanceledMaterial()); else if (Terminal != ETaskState::Succeeded) Self->Complete(FailedMaterial()); }, {.CancellationToken = State->CpuCancellation.GetToken(), .Priority = Priority});
		if (!Dispatch.HasValue()) Complete(FailedMaterial());
		{
			std::lock_guard Lock(State->Mutex);
			if (Dispatch.HasValue()) { auto Task = std::move(Dispatch).TakeValue(); if (!State->TaskId) State->TaskId = Task.GetTaskId(); State->Owner.TrackTask(std::move(Task)); }
		}
	}
	auto FMaterialBuildOperation::Start(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force,
		FTaskCancellationToken Cancellation, std::function<EMaterialTranslationAction(FMaterialTranslatedProgram&)> OnTranslation,
		std::function<void(FMaterialCompilerResult)> Completion, std::shared_ptr<const FMaterialTranslationInput> EncodedInput) -> void
	{
		DerivedData::FRequestBarrier Submission(State->Owner);
		const bool Admitted = TryReserve();
		{
			std::lock_guard Lock(State->Mutex); require(!State->Started); State->Started = true;
			State->Inline = !IsTaskSchedulerRunning(); State->Input = std::move(Input); State->EncodedInput = std::move(EncodedInput); State->Force = Force;
			State->Cancellation = State->Cancellation.WithObserver([Parent = std::move(Cancellation)] { return Parent.IsCancellationRequested(); });
			State->OnTranslation = std::move(OnTranslation); State->Completion = std::move(Completion);
		}
		if (!Admitted) return Complete(FailedMaterial());
		Launch([Self = shared_from_this()] {
			FMaterialError ValidationError;
			auto Admitted = SubmitTranslation(Self->State->Input, Self->State->Force, Self->State->Owner,
				[Self](FMaterialTranslatedProgram Product) { try { Self->Translated(std::move(Product)); } catch (...) { Self->Complete(FailedMaterial()); } }, Self->State->Cancellation, true, Self->State->EncodedInput, Self->State->Inline ? DerivedData::EBuildCacheExecution::Inline : DerivedData::EBuildCacheExecution::Task, &ValidationError);
			if (!Admitted) Self->Complete(ValidationError.HasError() ? FMaterialCompilerResult{.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Normalization, .Error = std::move(ValidationError)}}} : FailedMaterial(Admitted.error().Description));
		});
	}
	auto FMaterialBuildOperation::Translated(FMaterialTranslatedProgram Translation) -> void
	{
		if (State->Cancellation.IsCancelled()) return Complete(CanceledMaterial());
		std::function<EMaterialTranslationAction(FMaterialTranslatedProgram&)> Decide;
		{ std::lock_guard Lock(State->Mutex); Decide = std::move(State->OnTranslation); }
		if (Decide && Decide(Translation) != EMaterialTranslationAction::Compile) return Complete({}, ECompletionDelivery::Handled);
		if (!Translation) return Complete(AssembleMaterialProgram(std::move(Translation), {}));
		auto* ShaderBuild = IShaderBuildModule::Get();
		if (!ShaderBuild) return Complete(FailedMaterial());
		auto Product = std::make_shared<FMaterialTranslatedProgram>(std::move(Translation));
		const auto Begin = std::chrono::steady_clock::now();
		DerivedData::FRequestBarrier Submission(State->Owner);
		EShaderBuildPriority Priority;
		{ std::lock_guard Lock(State->Mutex); Priority = State->HighPriority ? EShaderBuildPriority::High : EShaderBuildPriority::Normal; }
		auto Submitted = ShaderBuild->SubmitGenerated(MakeMaterialShaderCompileRequest(*Product, State->Force),
			{.Priority = Priority, .IsCancelled = [Cancellation = State->Cancellation] { return Cancellation.IsCancelled(); }, .bInlineExecution = State->Inline},
			[Self = shared_from_this(), Product, Begin](FShaderBuildResult Output) {
				Product->Timings.CompilationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count();
				Self->Launch([Self, Product, Output = std::move(Output)]() mutable {
					if (Self->State->Cancellation.IsCancelled()) return Self->Complete(CanceledMaterial());
					Self->Complete(AssembleMaterialProgram(std::move(*Product), std::move(Output)));
				});
			});
		std::shared_ptr<FShaderBuildHandle> Shader;
		if (Submitted) Shader = std::make_shared<FShaderBuildHandle>(std::move(*Submitted));
		else Complete(FailedMaterial(FormatShaderBuildAdmissionError(Submitted.error())));
		if (Shader) State->Owner.TrackDependency({
			.IsFinished = [Shader] { return Shader->IsComplete(); },
			.Wait = [Shader] { return Shader->Wait() == EShaderBuildWaitResult::Completed ? DerivedData::ERequestWaitResult::Completed : DerivedData::ERequestWaitResult::WouldBlock; },
			.RequestCancel = [Shader] { Shader->RequestCancel(); }});
		{
			std::lock_guard Lock(State->Mutex); State->Shader = Shader; if (Shader && State->HighPriority) Shader->SetPriority(EShaderBuildPriority::High);
		}
		if (State->Cancellation.IsCancelled() && Shader) Shader->RequestCancel();
	}
	auto FMaterialBuildOperation::Complete(FMaterialCompilerResult Result, ECompletionDelivery Delivery) -> void
	{
		DerivedData::FRequestBarrier DeliveryBarrier(State->Owner);
		std::function<void(FMaterialCompilerResult)> Callback;
		{
			std::lock_guard Lock(State->Mutex); if (State->ResultReady) return; State->ResultReady = true;
			Callback = std::move(State->Completion); State->OnTranslation = {};
		}
		if (Result && EstimateProgramBytes(Result) > MaterialCompileMaxResultBytes) Result = {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds, .Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
		if (State->Cancellation.IsCancelled()) Result = CanceledMaterial();
		if (Delivery == ECompletionDelivery::Deliver && Callback) {
			std::function<void(EMaterialQualityLevel, bool)> Hook;
			{ std::lock_guard Lock(CompletionHookMutex); Hook = CompletionHook; }
			const auto Quality = Result.Quality;
			if (Hook) Hook(Quality, true);
			Callback(std::move(Result));
			if (Hook) Hook(Quality, false);
		}
	}
	auto FMaterialBuildOperation::Cancel() -> void
	{
		State->Cancellation.Cancel(); State->CpuCancellation.RequestCancellation(); State->Owner.RequestCancel();
	}
	auto FMaterialBuildOperation::IsFinished() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return State->Started && State->ResultReady && State->Owner.Poll();
	}
	auto FMaterialBuildOperation::GetTaskId() const -> uint64 { std::lock_guard Lock(State->Mutex); return State->TaskId; }
	auto FMaterialBuildOperation::Wait() -> void
	{
		RaisePriority();
		require(State->Owner.Wait() == DerivedData::ERequestWaitResult::Completed);
		require(IsFinished());
	}
	auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult
	{
		if (EstimateTranslationCaptureBytes(Input) > TranslationInputBytes) return {.Program = FailedMaterial()};
		auto Operation = std::make_shared<FMaterialBuildOperation>();
		if (!Operation->TryReserve()) return {.Program = FailedMaterial()};
		FMaterialBuildResult Result;
		Operation->Start(std::make_shared<const MIR::FCompilerInput>(Input), Force, {},
			[Force, &Input, &Result](auto& Product) {
				for (const auto& Location : Product.Sources) Result.Sources.push_back(Location.Source);
				if (Product && !Force) if (auto Retained = ConfigureProgram(FindProgram(Product.Identity), Input))
				{ Result.Program = *Retained; Result.Shared = std::move(Retained); Result.RetainedHit = true; return EMaterialTranslationAction::Reuse; }
				return EMaterialTranslationAction::Compile;
			}, [&](auto Value) { Result.Program = std::move(Value); });
		Operation->Wait();
		if (Result.Program && !Result.Shared) Result.Shared = RetainProgram(std::make_shared<const FMaterialCompilerResult>(Result.Program), Force);
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
		std::function<EMaterialTranslationAction(FMaterialTranslatedProgram&)>, std::function<void(FMaterialCompilerResult)> Completion, std::shared_ptr<const FMaterialTranslationInput> EncodedInput) -> void
	{ Completion(CompileMaterialInput({}, false).Program); }
	auto FMaterialBuildOperation::TryReserve() -> bool { return false; }
	auto FMaterialBuildOperation::RaisePriority() -> void {}
	auto FMaterialBuildOperation::Cancel() -> void {}
	auto FMaterialBuildOperation::Wait() -> void {}
	auto FMaterialBuildOperation::IsFinished() const -> bool { return true; }
	auto FMaterialBuildOperation::GetTaskId() const -> uint64 { return 0; }
	auto CompileMaterialInput(const MIR::FCompilerInput&, bool) -> FMaterialBuildResult
	{ return {.Program = {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::NoAuthoredProgram}}}}; }
}
#endif

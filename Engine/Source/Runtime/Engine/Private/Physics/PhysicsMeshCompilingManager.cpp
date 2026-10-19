#include "Physics/PhysicsMeshCompilation.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetBuildScheduler.h"
#include "Asset/AssetBuildRetirement.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCookHelper.h"
#include "Physics/PhysicsMeshInputTask.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#include "PhysicsCookInputPrivate.h"
#endif
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		using FClock = std::chrono::steady_clock;
		auto CheckOwnerThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}
		enum class EWorkPhase : uint8
		{
			Queued,
			Running,
			AwaitingDerivedData,
			Complete
		};
		struct FPhysicsWork : std::enable_shared_from_this<FPhysicsWork>
		{
			std::unique_ptr<FPhysicsMeshInputTask> InputTask;
			FCookBodySetupInfo CookInfo;
#if DURIN_WITH_EDITOR
			std::shared_ptr<DerivedData::FRequestOwner> BuildOperation = std::make_shared<DerivedData::FRequestOwner>();
			std::optional<DerivedData::FBuildCompleteParams> Completion;
#endif
			FCancellationToken Cancellation;
			std::atomic<EWorkPhase> Phase = EWorkPhase::Queued;
			std::expected<FPhysicsCookResult, std::string> Outcome;
			auto Build(const FAssetBuildTaskContext& Control, bool bDispatch) -> bool
			{
#if DURIN_WITH_EDITOR
				if (!bDispatch)
				{
					if (Completion) Outcome = PhysicsPrivate::ConsumeCookBuild(CookInfo.Mode, CookInfo.Policy, std::move(*Completion), Control);
					Completion.reset();
					return false;
				}
#endif
				auto Data = InputTask->Execute(Control);
				if (Control.IsCancelled()) return false;
				if (!Data) { Outcome = std::unexpected(Data.error()); return false; }
				CookInfo.TriangleMeshDesc = std::move(*Data);
				auto Input = FPhysicsCookHelper::Capture(std::move(CookInfo), Control);
				if (Control.IsCancelled()) return false;
				if (!Input) { Outcome = std::unexpected(Input.error()); return false; }
#if DURIN_WITH_EDITOR
				InputTask.reset();
				CookInfo.TriangleMeshDesc = {};
				if (Input->GetMode() == EBodySetupCollisionSourceMode::None) return false;
				Phase.store(EWorkPhase::AwaitingDerivedData, std::memory_order_release);
				auto Dispatched = PhysicsPrivate::DispatchCookBuild(*Input, *BuildOperation, Control, [Work = shared_from_this()](auto Value) {
					try { Work->Completion.emplace(std::move(Value)); }
					catch (...) { Work->Outcome = std::unexpected(MakeAssetBuildDiagnostic("Physics cache completion could not be retained.", "Cook")); }
					if (Work->Completion && Work->Completion->GetStatus() == DerivedData::EStatus::Canceled) Work->Cancellation.Cancel();
				});
				if (Dispatched) return true;
				Outcome = std::unexpected(std::move(Dispatched.error()));
#else
				Outcome = FPhysicsCookHelper::CookCaptured(*Input, Control);
#endif
				return false;
			}
			auto ReleaseInput() -> void
			{
				InputTask.reset();
				CookInfo = {};
			}
		};
		struct FPhysicsRecord
		{
			FObjectKey Body, AssetOwner;
			uint64 Generation = 0, EstimatedWorkingSetBytes = 0;
			EBodySetupCollisionSourceMode Mode;
			std::shared_ptr<FPhysicsWork> Work;
			FTaskHandle Task;
			FOnAsyncPhysicsCookFinished Completion;
		};
	} // namespace

	class FPhysicsMeshCompilingManager final : public IAssetCompilingManager,
											   public std::enable_shared_from_this<FPhysicsMeshCompilingManager>
	{
	public:
		auto Start() -> FAssetCompilerStartResult override
		{
			CheckOwnerThread();
			if (bAccepting) return {};
			if (!Records.empty()) return {EAssetCompilerStartError::Undrained, Records.size()};
			if (!IsTaskSchedulerRunning()) return {EAssetCompilerStartError::SchedulerUnavailable};
			bAccepting = true;
			return {};
		}
		auto StopAdmission() -> void override
		{
			CheckOwnerThread();
			bAccepting = false;
			for (const auto& Record : Records)
				Cancel(*Record);
		}
		auto GetNumRemainingAssets() const -> uint64 override
		{
			CheckOwnerThread();
			return Records.size();
		}
		auto HasPending(const DBodySetup& Body) const -> bool
		{
			CheckOwnerThread();
			return std::ranges::any_of(Records, [&](const auto& Record) { return Record->Body == FObjectKey(const_cast<DBodySetup*>(&Body)); });
		}
		auto Diagnostics() const -> FPhysicsMeshCompilationManagerDiagnostics
		{
			CheckOwnerThread();
			const auto Running = std::ranges::count_if(Records, [](const auto& Record) { return Record->Work->Phase.load(std::memory_order_acquire) == EWorkPhase::Running; });
			uint64 EstimatedBytes = 0;
			for (const auto& Record : Records) EstimatedBytes += Record->EstimatedWorkingSetBytes;
			return {uint32(Records.size()), uint32(Running), EstimatedBytes, bAccepting};
		}
		auto Submit(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask, bool bPersist, FOnAsyncPhysicsCookFinished Completion)
			-> void
		{
			CheckOwnerThread();
			const auto Reject = [&](std::string Message) {
				const auto Generation = Body.GetPhysicsMeshRequestGeneration();
				Body.FailPhysicsMeshes(Generation, MakeAssetBuildDiagnostic(std::move(Message), "Scheduling"));
				const FPhysicsCookCompletionResult Result{.Status = EPhysicsCookCompletionStatus::Failed, .Error = Body.GetPhysicsMeshBuildError()};
				if (Completion) Completion(Result);
			};
			if (!bAccepting || !IsValid(&Body) || Body.IsCompilationMutationBlocked())
				return Reject("Physics mesh compilation is not accepting this owner.");
			if (!InputTask || InputTask->GetWorkingSetBytes() == 0 || InputTask->GetWorkingSetBytes() > MaximumAssetBuildRequestBytes)
				return Reject("Physics mesh preparation requires a bounded detached task.");
			const auto Bytes = InputTask->GetWorkingSetBytes();
			auto Record = std::make_shared<FPhysicsRecord>();
			Record->Body = FObjectKey(&Body);
			Record->AssetOwner = FObjectKey(Body.GetOuter());
			Record->Generation = Body.GetPhysicsMeshRequestGeneration();
			Record->Mode = Body.GetCollisionSourceMode();
			Record->EstimatedWorkingSetBytes = Bytes;
			Record->Work = std::make_shared<FPhysicsWork>();
			Record->Work->InputTask = std::move(InputTask);
			Record->Work->CookInfo = Body.GetCookInfo({}, bPersist);
			Record->Completion = std::move(Completion);
			Records.push_back(Record);
			Enqueue(Record);
		}
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
		{
			CheckOwnerThread();
			for (auto* Object : Objects)
				for (const auto& Record : Records)
					if (Record->Body == FObjectKey(Object) || Record->AssetOwner == FObjectKey(Object)) Cancel(*Record);
		}
		auto FinishCompilationForObjects(std::span<DObject* const> Objects) -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			std::vector<FObjectKey> Owners;
			for (auto* Object : Objects)
				Owners.emplace_back(Object);
			return Owners.empty() ? FAssetCompileProcessResult{} : Finish(Owners);
		}
		auto FinishAllCompilation() -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			return Finish({});
		}
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			return Pump({}, Params.MaximumCompletions, Params.Deadline.value_or(FClock::time_point::max()));
		}
		auto Shutdown() -> void override
		{
			CheckOwnerThread();
			StopAdmission();
			FinishAllCompilation();

		}
		~FPhysicsMeshCompilingManager() override { Shutdown(); }
		std::function<void()> BeforeBuild, AfterBuild;

	private:
		static auto Selected(const FPhysicsRecord& Record, std::span<const FObjectKey> Owners) -> bool
		{
			return Owners.empty() || std::ranges::contains(Owners, Record.Body) || std::ranges::contains(Owners, Record.AssetOwner);
		}
		static auto Cancel(FPhysicsRecord& Record) -> void
		{
			if (auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record.Body))) Body->CancelPhysicsMeshes(Record.Generation);
			// Compete with worker entry, never interrupt a cook that has started.
			auto Expected = EWorkPhase::Queued;
			if (Record.Work->Phase.compare_exchange_strong(Expected, EWorkPhase::Complete))
			{
				Record.Work->Cancellation.Cancel();
				if (Record.Task.IsValid()) CancelTask(Record.Task);
				Record.Work->ReleaseInput();
			}
		}
		auto Enqueue(const std::shared_ptr<FPhysicsRecord>& Record, bool bDispatch = true, FTaskHandle Prerequisite = {}) -> void
		{
			std::function<bool()> IsReady;
#if DURIN_WITH_EDITOR
			if (!bDispatch) IsReady = [WeakRecord = std::weak_ptr(Record)] {
				const auto Record = WeakRecord.lock();
				return !Record || Record->Work->BuildOperation->Poll();
			};
#endif
			Scheduler->Enqueue(false, Record->EstimatedWorkingSetBytes,
				[Manager = weak_from_this(), WeakRecord = std::weak_ptr(Record), bDispatch](std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> bool {
					auto Self = Manager.lock();
					auto Record = WeakRecord.lock();
					if (!Self || !Record) return false;
					if (bDispatch)
					{
						if (Record->Work->Cancellation.IsCancellationRequested() || Record->Work->Phase.load() != EWorkPhase::Queued) return false;
						if (!IsTaskSchedulerRunning()) { Self->Cancel(*Record); return false; }
					}
#if DURIN_WITH_EDITOR
					else
					{
						if (Record->Work->Phase.load() != EWorkPhase::AwaitingDerivedData || RetireDerivedDataWithoutCpu(*Record)) return false;
					}
#endif
					if (!bDispatch) Record->Work->Phase.store(EWorkPhase::Running, std::memory_order_release);
					return Self->Launch(Record, bDispatch, std::move(Slot)).IsValid();
				}, std::move(Prerequisite), std::move(IsReady));
		}
#if DURIN_WITH_EDITOR
		static auto RetireDerivedDataWithoutCpu(FPhysicsRecord& Record) -> bool
		{
			auto& Work = *Record.Work;
			if (!Work.BuildOperation->Poll()) return false;
			if (!IsTaskSchedulerRunning() || Record.Task.GetState() == ETaskState::Canceled) Work.Cancellation.Cancel();
			if (!Work.Cancellation.IsCancelled() && Work.Completion) return false;
			Work.Completion.reset();
			if (Work.Cancellation.IsCancelled()) Work.Outcome = FPhysicsCookResult{};
			Work.ReleaseInput();
			Work.Phase.store(EWorkPhase::Complete, std::memory_order_release);
			return true;
		}
#endif
		auto Launch(const std::shared_ptr<FPhysicsRecord>& Record, bool bDispatch, std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> FTaskHandle
		{
			Tasks::FTaskExecutionOptions Options;
			Record->Task = Tasks::LaunchTask("PhysicsMesh.Cook", [Slot = std::move(Slot), Work = Record->Work, Bytes = Record->EstimatedWorkingSetBytes, Before = BeforeBuild, After = AfterBuild, bDispatch](const FTaskCancellationToken& NativeToken) {
				if (bDispatch)
				{
					auto Expected = EWorkPhase::Queued;
					if (!Work->Phase.compare_exchange_strong(Expected, EWorkPhase::Running)) return;
				}
				const auto Cancellation = Work->Cancellation.WithObserver([NativeToken] { return NativeToken.IsCancellationRequested(); });
				try
				{
					if (bDispatch && Before) Before();
					const bool Waiting = Work->Build({.Cancellation = Cancellation, .MaximumWorkingSetBytes = Bytes}, bDispatch);
#if DURIN_WITH_EDITOR
					if (Waiting)
					{
						return;
					}
#endif
					if (After) After();
				}
				catch (...) { Work->Outcome = std::unexpected(MakeAssetBuildDiagnostic("Physics mesh worker failed with an exception.", "Cook")); }
				if (Cancellation.IsCancelled()) Work->Outcome = FPhysicsCookResult{};
				Work->ReleaseInput(); Work->Phase.store(EWorkPhase::Complete, std::memory_order_release); }, Options);
#if DURIN_WITH_EDITOR
			if (bDispatch) Enqueue(Record, false, Record->Task);
#endif
			return Record->Task;
		}
		auto Pump(std::span<const FObjectKey> Owners, uint32 Maximum, FClock::time_point Deadline) -> FAssetCompileProcessResult
		{
			Scheduler->Pump();
			for (const auto& Record : Records)
			{
				if (!Record->Task.IsValid() || !Record->Task.IsComplete()) continue;
#if DURIN_WITH_EDITOR
				if (!Record->Work->BuildOperation->Poll()) continue;
#endif
				const auto Phase = Record->Work->Phase.load(std::memory_order_acquire);
#if DURIN_WITH_EDITOR
				if (Phase == EWorkPhase::AwaitingDerivedData) { RetireDerivedDataWithoutCpu(*Record); continue; }
#endif
				if (Phase != EWorkPhase::Running && Phase != EWorkPhase::Queued) continue;
				AssetPrivate::ObserveAssetBuildTaskCancellation(Record->Task, Record->Work->Cancellation);
				if (Record->Task.GetState() != ETaskState::Canceled) Record->Work->Outcome = std::unexpected(MakeAssetBuildDiagnostic("Physics mesh task retired before worker completion.", "Cook"));
				Record->Work->ReleaseInput();
				Record->Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
			}
			Scheduler->Pump();
			FAssetCompileProcessResult Result;
			const auto Pending = Records;
			for (const auto& Record : Pending)
			{
				if (Result.ProcessedCompletionCount >= Maximum || FClock::now() >= Deadline) break;
				if (!Selected(*Record, Owners) || !std::ranges::contains(Records, Record)
					|| Record->Work->Phase.load(std::memory_order_acquire) != EWorkPhase::Complete
					|| (Record->Task.IsValid() && !Record->Task.IsComplete())) continue;
#if DURIN_WITH_EDITOR
				if (!Record->Work->BuildOperation->Poll()) continue;
#endif
				std::erase(Records, Record);
				auto Outcome = std::exchange(Record->Work->Outcome, FPhysicsCookResult{});
#if DURIN_WITH_EDITOR
				[[maybe_unused]] auto DdcCompletion = std::exchange(Record->Work->Completion, std::nullopt);
#endif
				auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record->Body));
				FPhysicsCookCompletionResult Completion;
				if (Record->Work->Cancellation.IsCancelled() || !Body)
				{
					Cancel(*Record);
					Completion.Status = EPhysicsCookCompletionStatus::Cancelled;
				}
				else if (Body->GetPhysicsMeshRequestGeneration() != Record->Generation
					|| Body->GetCollisionSourceMode() != Record->Mode)
					Completion.Status = EPhysicsCookCompletionStatus::Superseded;
				else if (!Outcome)
				{
					Body->FailPhysicsMeshes(Record->Generation, Outcome.error());
					Completion.Error = Body->GetPhysicsMeshBuildError();
				}
				else
				{
					const auto& Product = *Outcome;
					Completion.Status = Body->ApplyPhysicsMeshes(Record->Generation, Product.Simple, Product.Complex);
					if (Completion.Status == EPhysicsCookCompletionStatus::Failed) Completion.Error = Body->GetPhysicsMeshBuildError();
				}
				if (Body && Completion.Status != EPhysicsCookCompletionStatus::Succeeded) Body->CancelPhysicsMeshes(Record->Generation);
				++Result.ProcessedCompletionCount;
				if (auto Notify = std::move(Record->Completion))
				{
					Notify(Completion);
				}
			}
			Scheduler->Pump();
			return Result;
		}
		auto Finish(std::span<const FObjectKey> Owners) -> FAssetCompileProcessResult
		{
			FAssetCompileProcessResult Result;
			while (std::ranges::any_of(Records, [&](const auto& Record) { return Selected(*Record, Owners); }))
			{
				auto Batch = Pump(Owners, std::numeric_limits<uint32>::max(), FClock::time_point::max());
				Result.ProcessedCompletionCount += Batch.ProcessedCompletionCount;
				if (!Batch.ProcessedCompletionCount)
				{
					bool JoinedOwner = false;
#if DURIN_WITH_EDITOR
					for (const auto& Record : Records)
						if (Selected(*Record, Owners) && AssetPrivate::WaitForAssetBuildOwner(Record->Task, *Record->Work->BuildOperation))
						{ JoinedOwner = true; break; }
#endif
					if (!JoinedOwner) Scheduler->WaitForProgress();
				}
			}
			return Result;
		}
		std::vector<std::shared_ptr<FPhysicsRecord>> Records;
		std::shared_ptr<AssetPrivate::FAssetBuildScheduler> Scheduler = AssetPrivate::GetAssetBuildScheduler();
		bool bAccepting = false;
	};
	namespace
	{
		std::weak_ptr<FPhysicsMeshCompilingManager> GPhysicsManager;
	}
	namespace AssetPrivate
	{
		auto CreatePhysicsMeshCompilingManager() -> std::shared_ptr<IAssetCompilingManager>
		{
			CheckOwnerThread();
			if (auto Manager = GPhysicsManager.lock()) return Manager;
			auto Manager = std::make_shared<FPhysicsMeshCompilingManager>();
			GPhysicsManager = Manager;
			return Manager;
		}
		auto CancelPhysicsMeshCompilationForOwner(DObject& Owner) -> void
		{
			CheckOwnerThread();
			DObject* Object = &Owner;
			if (auto Manager = GPhysicsManager.lock()) Manager->MarkCompilationAsCanceled({&Object, 1});
		}
		auto FinishPhysicsMeshCompilationForOwner(DObject& Owner) -> void
		{
			CheckOwnerThread();
			DObject* Object = &Owner;
			if (auto Manager = GPhysicsManager.lock()) Manager->FinishCompilationForObjects({&Object, 1});
		}
		auto SetPhysicsMeshCompilationHooksForTests(std::function<void()> Before, std::function<void()> After) -> void
		{
			CheckOwnerThread();
			if (auto Manager = GPhysicsManager.lock())
			{
				Manager->BeforeBuild = std::move(Before);
				Manager->AfterBuild = std::move(After);
			}
		}
	} // namespace AssetPrivate
	auto SubmitPhysicsMeshCompilation(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask, bool bPersist, FOnAsyncPhysicsCookFinished Completion)
		-> void
	{
		CheckOwnerThread();
		if (auto Manager = GPhysicsManager.lock()) return Manager->Submit(Body, std::move(InputTask), bPersist, std::move(Completion));
		Body.FailPhysicsMeshes(Body.GetPhysicsMeshRequestGeneration(), MakeAssetBuildDiagnostic("Physics mesh compiling service is unavailable.", "Scheduling"));
		const FPhysicsCookCompletionResult Result{.Status = EPhysicsCookCompletionStatus::Failed, .Error = Body.GetPhysicsMeshBuildError()};
		if (Completion) Completion(Result);
	}
	auto CancelPhysicsMeshCompilation(DBodySetup& Body) -> void { AssetPrivate::CancelPhysicsMeshCompilationForOwner(Body); }
	auto FinishPhysicsMeshCompilation(DBodySetup& Body) -> void { AssetPrivate::FinishPhysicsMeshCompilationForOwner(Body); }
	auto IsPhysicsMeshCompiling(const DBodySetup& Body) -> bool
	{
		CheckOwnerThread();
		auto Manager = GPhysicsManager.lock();
		return Manager && Manager->HasPending(Body);
	}
	auto GetPhysicsMeshCompilationManagerDiagnostics() -> FPhysicsMeshCompilationManagerDiagnostics
	{
		CheckOwnerThread();
		auto Manager = GPhysicsManager.lock();
		return Manager ? Manager->Diagnostics() : FPhysicsMeshCompilationManagerDiagnostics{};
	}
} // namespace Durin

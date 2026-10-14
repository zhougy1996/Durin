#include "Physics/PhysicsMeshCompilation.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/MeshBuildBudget.h"
#include "Asset/AssetBuildScheduler.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCookHelper.h"
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
			Complete
		};
		struct FPhysicsWork
		{
			std::unique_ptr<FPhysicsMeshInputTask> InputTask;
			FCookBodySetupInfo CookInfo;
			FCancellationToken Cancellation;
			std::atomic<EWorkPhase> Phase = EWorkPhase::Queued;
			std::expected<FPhysicsCookResult, std::string> Outcome;
			auto Build(const FAssetBuildTaskContext& Control) -> void
			{
				auto Data = InputTask->Execute(Control);
				if (Control.IsCancelled()) return;
				if (!Data)
				{
					Outcome = std::unexpected(Data.error());
					return;
				}
				CookInfo.TriangleMeshDesc = std::move(*Data);
				auto Input = FPhysicsCookHelper::Capture(std::move(CookInfo), Control);
				if (Control.IsCancelled()) return;
				if (!Input)
				{
					Outcome = std::unexpected(Input.error());
					return;
				}
				Outcome = FPhysicsCookHelper::CookCaptured(*Input, Control);
			}
			auto ReleaseInput() -> void
			{
				InputTask.reset();
				CookInfo = {};
			}
		};
		struct FPhysicsRecord
		{
			std::unique_ptr<AssetPrivate::FMeshBuildReservation> Reservation;
			FObjectKey Body, AssetOwner;
			uint64 Revision = 0, Generation = 0, ReservedBytes = 0;
			EBodySetupCollisionSourceMode Mode;
			EBodySetupCollisionQueryPolicy Policy;
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
			Scope = CreateTaskScope();
			bAccepting = Scope.IsValid();
			return {bAccepting ? EAssetCompilerStartError::None : EAssetCompilerStartError::TaskScopeUnavailable};
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
			return {Accounting->RecordCount, uint32(Running), Accounting->ReservedBytes, bAccepting};
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
			if (!InputTask || InputTask->GetWorkingSetBytes() == 0 || InputTask->GetWorkingSetBytes() > Budget->MaximumRequestBytes)
				return Reject("Physics mesh preparation requires a bounded detached task.");
			const auto Bytes = InputTask->GetWorkingSetBytes();
			auto Record = std::make_shared<FPhysicsRecord>();
			Record->Reservation = AssetPrivate::FMeshBuildReservation::TryCreate(Budget, Accounting, Bytes);
			if (!Record->Reservation) return Reject("Physics mesh compilation admission budget exhausted.");
			Record->Body = FObjectKey(&Body);
			Record->AssetOwner = FObjectKey(Body.GetOuter());
			Record->Revision = Body.GetRevision();
			Record->Generation = Body.GetPhysicsMeshRequestGeneration();
			Record->Mode = Body.GetCollisionSourceMode();
			Record->Policy = Body.GetCollisionQueryPolicy();
			Record->ReservedBytes = Bytes;
			Record->Work = std::make_shared<FPhysicsWork>();
			Record->Work->InputTask = std::move(InputTask);
			Record->Work->CookInfo = Body.GetCookInfo({}, bPersist);
			Record->Completion = std::move(Completion);
			Records.push_back(Record);
			Scheduler->Enqueue(false, [Manager = weak_from_this(), WeakRecord = std::weak_ptr(Record)](std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> FTaskHandle {
				auto Self = Manager.lock();
				auto Record = WeakRecord.lock();
				if (!Self || !Record || Record->Work->Cancellation.IsCancellationRequested() || Record->Work->Phase.load() != EWorkPhase::Queued) return {};
				if (!IsTaskSchedulerRunning())
				{
					Self->Cancel(*Record);
					return {};
				}
				Record->Work->Phase.store(EWorkPhase::Running, std::memory_order_release);
				return Self->Launch(Record, std::move(Slot));
			});
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
			if (Scope.IsValid())
			{
				Scope.Close(ETaskScopeCloseMode::Drain);
				Scope.Wait();
			}
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
			Record.Work->Cancellation.Cancel();
			if (auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record.Body))) Body->CancelPhysicsMeshes(Record.Generation);
			if (Record.Work->Phase.load() == EWorkPhase::Queued)
			{
				Record.Work->ReleaseInput();
				Record.Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
			}
		}
		auto Launch(const std::shared_ptr<FPhysicsRecord>& Record, std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> FTaskHandle
		{
			Tasks::FTaskExecutionOptions Options;
			Options.Scope = Scope.GetToken();
			Record->Task = Tasks::LaunchTask("PhysicsMesh.Cook", [Slot = std::move(Slot), Work = Record->Work, Bytes = Record->ReservedBytes, Before = BeforeBuild, After = AfterBuild](const FTaskCancellationToken& NativeToken) {
				AssetPrivate::FAssetBuildExecutionSlot::FWorkScope Execution{*Slot};
				const auto Cancellation = Work->Cancellation.WithObserver([NativeToken] { return NativeToken.IsCancellationRequested(); });
				try
				{ if (Before) Before(); Work->Build({.Cancellation = Cancellation, .MaximumWorkingSetBytes = Bytes}); if (After) After(); }
				catch (...) { Work->Outcome = std::unexpected(MakeAssetBuildDiagnostic("Physics mesh worker failed with an exception.", "Cook")); }
				if (Cancellation.IsCancelled()) Work->Outcome = FPhysicsCookResult{};
				Work->ReleaseInput(); Work->Phase.store(EWorkPhase::Complete, std::memory_order_release); }, Options).GetTaskHandle();
			return Record->Task;
		}
		auto Pump(std::span<const FObjectKey> Owners, uint32 Maximum, FClock::time_point Deadline) -> FAssetCompileProcessResult
		{
			Scheduler->Pump();
			for (const auto& Record : Records)
				if (Record->Work->Phase.load() == EWorkPhase::Running && Record->Task.IsValid() && Record->Task.IsComplete())
				{
					if (Record->Task.GetState() == ETaskState::Canceled)
						Record->Work->Cancellation.Cancel();
					else
						Record->Work->Outcome = std::unexpected(MakeAssetBuildDiagnostic("Physics mesh task retired before worker completion.", "Cook"));
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
				std::erase(Records, Record);
				auto Reservation = std::move(Record->Reservation);
				struct FRetireOutput
				{
					const std::shared_ptr<FPhysicsRecord>& Record;
					~FRetireOutput()
					{
						Record->Work->Outcome = FPhysicsCookResult{};
					}
				} Retirement{Record};
				auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record->Body));
				FPhysicsCookCompletionResult Completion;
				if (Record->Work->Cancellation.IsCancelled() || !Body)
				{
					Cancel(*Record);
					Completion.Status = EPhysicsCookCompletionStatus::Cancelled;
				}
				else if (Body->GetRevision() != Record->Revision || Body->GetPhysicsMeshRequestGeneration() != Record->Generation
					|| Body->GetCollisionSourceMode() != Record->Mode || Body->GetCollisionQueryPolicy() != Record->Policy)
					Completion.Status = EPhysicsCookCompletionStatus::Superseded;
				else if (!Record->Work->Outcome)
				{
					Body->FailPhysicsMeshes(Record->Generation, Record->Work->Outcome.error());
					Completion.Error = Body->GetPhysicsMeshBuildError();
				}
				else
				{
					const auto& Product = *Record->Work->Outcome;
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
					Scheduler->WaitForProgress();
				}
			}
			return Result;
		}
		std::shared_ptr<AssetPrivate::FMeshBuildBudget> Budget = AssetPrivate::GetMeshBuildBudget();
		std::vector<std::shared_ptr<FPhysicsRecord>> Records;
		FTaskScope Scope;
		std::shared_ptr<AssetPrivate::FAssetBuildScheduler> Scheduler = AssetPrivate::GetAssetBuildScheduler();
		std::shared_ptr<AssetPrivate::FMeshBuildAccounting> Accounting = std::make_shared<AssetPrivate::FMeshBuildAccounting>();
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

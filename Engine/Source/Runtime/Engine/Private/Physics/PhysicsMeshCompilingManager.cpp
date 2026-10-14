#include "Physics/PhysicsMeshCompilation.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/MeshBuildBudget.h"
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
			std::expected<FPhysicsCookResult, FPhysicsCookFailure> Outcome;
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
			FObjectKey Body, AssetOwner;
			uint64 Revision = 0, Generation = 0, ReservedBytes = 0;
			EBodySetupCollisionSourceMode Mode;
			EBodySetupCollisionQueryPolicy Policy;
			std::shared_ptr<FPhysicsWork> Work;
			FTaskHandle Task;
			std::optional<EPhysicsCookCompletionStatus> Terminal;
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
			return {uint32(Records.size() + FinalizingCount), uint32(Running), ReservedBytes, bAccepting};
		}
		auto Submit(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask, bool bPersist, FOnAsyncPhysicsCookFinished Completion)
			-> std::expected<void, FPhysicsCookFailure>
		{
			CheckOwnerThread();
			const auto Reject = [](std::string Message) { return std::unexpected(FPhysicsCookFailure{std::move(Message), EPhysicsCookStage::Scheduling}); };
			if (!bAccepting || !IsValid(&Body) || Budget->MutatingOwners.contains(FObjectKey(&Body)) || Budget->MutatingOwners.contains(FObjectKey(Body.GetOuter())))
				return Reject("Physics mesh compilation is not accepting this owner.");
			if (!InputTask || InputTask->GetWorkingSetBytes() == 0 || InputTask->GetWorkingSetBytes() > Budget->MaximumRequestBytes)
				return Reject("Physics mesh preparation requires a bounded detached task.");
			const auto Bytes = InputTask->GetWorkingSetBytes();
			if (!Budget->CanReserve(Bytes)) return Reject("Physics mesh compilation admission budget exhausted.");
			auto Record = std::make_shared<FPhysicsRecord>();
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
			Budget->Reserve(Bytes);
			ReservedBytes += Bytes;
			Records.push_back(Record);
			Budget->Enqueue(false, [Manager = weak_from_this(), WeakRecord = std::weak_ptr(Record)] {
				auto Self = Manager.lock();
				auto Record = WeakRecord.lock();
				if (!Self || !Record || Record->Terminal || Record->Work->Phase.load() != EWorkPhase::Queued) return false;
				if (!IsTaskSchedulerRunning())
				{
					Self->Cancel(*Record);
					return false;
				}
				Record->Work->Phase.store(EWorkPhase::Running, std::memory_order_release);
				Self->Launch(Record);
				return true;
			});
			return {};
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
			Budget->BeginPump(Params.PumpIdentity);
			auto Result = Pump({}, std::min<uint32>(2 - Budget->PumpCount, Params.MaximumCompletions), std::min(Params.Deadline.value_or(FClock::time_point::max()), Budget->PumpDeadline));
			Budget->PumpCount = std::min<uint32>(2, Budget->PumpCount + Result.ProcessedCompletionCount);
			return Result;
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
			if (Record.Terminal) return;
			Record.Terminal = EPhysicsCookCompletionStatus::Cancelled;
			Record.Work->Cancellation.Cancel();
			if (auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record.Body))) Body->CancelPhysicsMeshes(Record.Generation);
			if (Record.Work->Phase.load() == EWorkPhase::Queued)
			{
				Record.Work->ReleaseInput();
				Record.Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
			}
		}
		auto Launch(const std::shared_ptr<FPhysicsRecord>& Record) -> void
		{
			Tasks::FTaskExecutionOptions Options;
			Options.Scope = Scope.GetToken();
			Record->Task = Tasks::LaunchTask("PhysicsMesh.Cook", [Work = Record->Work, Budget = Budget, Bytes = Record->ReservedBytes, Before = BeforeBuild, After = AfterBuild](const FTaskCancellationToken& NativeToken) {
				const auto Cancellation = Work->Cancellation.WithObserver([NativeToken] { return NativeToken.IsCancellationRequested(); });
				try
				{ if (Before) Before(); Work->Build({.Cancellation = Cancellation, .MaximumWorkingSetBytes = Bytes}); if (After) After(); }
				catch (...) { Work->Outcome = std::unexpected(FPhysicsCookFailure{"Physics mesh worker failed with an exception."}); }
				if (Cancellation.IsCancelled()) Work->Outcome = FPhysicsCookResult{};
				Work->ReleaseInput(); Work->Phase.store(EWorkPhase::Complete, std::memory_order_release); Budget->ReleaseSlot(); }, Options).GetTaskHandle();
		}
		auto Pump(std::span<const FObjectKey> Owners, uint32 Maximum, FClock::time_point Deadline) -> FAssetCompileProcessResult
		{
			if (Budget->ProgressRender) Budget->ProgressRender();
			for (const auto& Record : Records)
				if (Record->Work->Phase.load() == EWorkPhase::Running && Record->Task.IsValid() && Record->Task.IsComplete())
				{
					if (Record->Task.GetState() == ETaskState::Canceled)
						Record->Work->Cancellation.Cancel();
					else
						Record->Work->Outcome = std::unexpected(FPhysicsCookFailure{"Physics mesh task retired before worker completion."});
					Record->Work->ReleaseInput();
					Record->Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
					Budget->ReleaseSlot();
				}
			Budget->Admit();
			FAssetCompileProcessResult Result;
			const auto Pending = Records;
			for (const auto& Record : Pending)
			{
				if (Result.ProcessedCompletionCount >= Maximum || FClock::now() >= Deadline) break;
				if (!Selected(*Record, Owners) || !std::ranges::contains(Records, Record)
					|| Record->Work->Phase.load(std::memory_order_acquire) != EWorkPhase::Complete
					|| (Record->Task.IsValid() && !Record->Task.IsComplete())) continue;
				std::erase(Records, Record);
				++FinalizingCount;
				struct FRetirement
				{
					FPhysicsMeshCompilingManager& Manager;
					const std::shared_ptr<FPhysicsRecord>& Record;
					~FRetirement()
					{
						Record->Work->Outcome = FPhysicsCookResult{};
						Manager.ReservedBytes -= Record->ReservedBytes;
						Manager.Budget->Release(Record->ReservedBytes);
						--Manager.FinalizingCount;
					}
				} Retirement{*this, Record};
				auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record->Body));
				if (!Record->Terminal && Record->Work->Cancellation.IsCancelled()) Cancel(*Record);
				if (!Record->Terminal)
				{
					if (!Body)
						Record->Terminal = EPhysicsCookCompletionStatus::Cancelled;
					else if (Body->GetRevision() != Record->Revision || Body->GetPhysicsMeshRequestGeneration() != Record->Generation
							 || Body->GetCollisionSourceMode() != Record->Mode || Body->GetCollisionQueryPolicy() != Record->Policy)
						Record->Terminal = EPhysicsCookCompletionStatus::Superseded;
					else if (!Record->Work->Outcome)
					{
						Body->FailPhysicsMeshes(Record->Generation, Record->Work->Outcome.error());
						Record->Terminal = EPhysicsCookCompletionStatus::Failed;
					}
					else
					{
						const auto& Product = *Record->Work->Outcome;
						switch (Body->ApplyPhysicsMeshes(Record->Generation, Product.Simple, Product.Complex))
						{
						case EPhysicsMeshApplyResult::Applied: Record->Terminal = EPhysicsCookCompletionStatus::Succeeded; break;
						case EPhysicsMeshApplyResult::Superseded: Record->Terminal = EPhysicsCookCompletionStatus::Superseded; break;
						case EPhysicsMeshApplyResult::Failed:
							Record->Terminal = EPhysicsCookCompletionStatus::Failed;
							Record->Work->Outcome = std::unexpected(*Body->GetPhysicsMeshBuildError());
							break;
						}
					}
				}
				if (Body && *Record->Terminal != EPhysicsCookCompletionStatus::Succeeded) Body->CancelPhysicsMeshes(Record->Generation);
				++Result.ProcessedCompletionCount;
				if (auto Notify = std::move(Record->Completion))
				{
					FPhysicsCookCompletionResult Completion{.Status = *Record->Terminal};
					if (*Record->Terminal == EPhysicsCookCompletionStatus::Failed) Completion.Error = Record->Work->Outcome.error();
					Notify(Completion);
				}
			}
			Budget->Admit();
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
					std::unique_lock Lock(Budget->Mutex);
					Budget->Changed.wait_for(Lock, std::chrono::milliseconds(1));
				}
			}
			return Result;
		}
		std::shared_ptr<AssetPrivate::FMeshBuildBudget> Budget = AssetPrivate::GetMeshBuildBudget();
		std::vector<std::shared_ptr<FPhysicsRecord>> Records;
		FTaskScope Scope;
		uint64 ReservedBytes = 0;
		uint32 FinalizingCount = 0;
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
		-> std::expected<void, FPhysicsCookFailure>
	{
		CheckOwnerThread();
		if (auto Manager = GPhysicsManager.lock()) return Manager->Submit(Body, std::move(InputTask), bPersist, std::move(Completion));
		return std::unexpected(FPhysicsCookFailure{"Physics mesh compiling service is unavailable.", EPhysicsCookStage::Scheduling});
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

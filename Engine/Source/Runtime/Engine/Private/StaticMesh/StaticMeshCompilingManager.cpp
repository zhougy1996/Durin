#include "Threading/TaskComposition.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Asset/AssetCompilingManager.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMeshBuildPrivate.h"
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
	#include "DerivedDataRequestOwner.h"
#endif

#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Asset/AssetReadResult.h"
#include "Asset/AssetBuildScheduler.h"
#include "Asset/AssetBuildRetirement.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		using FClock = std::chrono::steady_clock;
		constexpr size_t MaximumHistory = 128;

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

		struct FWork
		{
#if DURIN_WITH_EDITORONLY_DATA
			FStaticMeshBuildRequest Request;
	#if DURIN_WITH_EDITOR
			std::shared_ptr<DerivedData::FRequestOwner> BuildOperation;
			std::optional<DerivedData::FBuildCompleteParams> Completion;
	#endif
#else
			std::monostate Request;
#endif
			std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Outcome =
				std::unexpected(MakeAssetBuildDiagnostic("StaticMesh render build has not started."));
			uint64 EstimatedWorkingSetBytes = 0;
			FCancellationToken Cancellation;
			std::atomic<EWorkPhase> Phase = EWorkPhase::Queued;
			auto Build(const FAssetBuildTaskContext& Control) -> void
			{
#if DURIN_WITH_EDITORONLY_DATA
	#if DURIN_WITH_EDITOR
				if (Completion) Outcome = StaticMeshPrivate::ConsumeRenderBuild(Request.Settings, std::move(*Completion), Control);
				Completion.reset();
	#else
				Outcome = BuildStaticMeshRenderData(std::move(Request), Control);
	#endif
#else
				Outcome = std::unexpected(MakeAssetBuildDiagnostic("Authored mesh builds require editor-only data."));
#endif
				if (Control.IsCancelled()) Outcome = std::unique_ptr<FStaticMeshRenderData>{};
			}
			auto Fail(std::string Message) -> void { Outcome = std::unexpected(MakeAssetBuildDiagnostic(std::move(Message))); }
			auto ReleaseInput() -> void { Request = {}; }
			auto ReleaseOutput() -> void { Outcome = std::unique_ptr<FStaticMeshRenderData>{}; }
		};

		struct FRecord
		{
			FStaticMeshCompilationDiagnostic Diagnostic;
			FStaticMeshReconciliationSnapshot Snapshot;
			std::optional<std::vector<FMeshMaterialSlotDefinition>> PreparedMaterialSlots;
			FObjectKey Package;
			FObjectKey ImportData;
#if DURIN_WITH_EDITORONLY_DATA
			FStaticMeshSource RequestedSource;
#endif
			bool bRequeue = false;
			std::optional<FXxHash128> ImportState;
			std::shared_ptr<FWork> Work;
			FTaskHandle Task;
			EStaticMeshCompilationPriority Priority;
			std::optional<EStaticMeshCompilationStatus> Terminal;
			bool bMarkPackageDirty = true;
			bool bPersistDerivedData = true;
		};

	} // namespace

	class FStaticMeshCompilingManager final : public IAssetCompilingManager, public std::enable_shared_from_this<FStaticMeshCompilingManager>
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
				Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
		}
		auto GetNumRemainingAssets() const -> uint64 override
		{
			CheckOwnerThread();
			return Records.size();
		}

#if DURIN_WITH_EDITORONLY_DATA
		auto Submit(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request) -> std::expected<void, std::string>
		{
			CheckOwnerThread();
			const auto Reject = [](std::string Message) -> std::expected<void, std::string> {
				Message.resize(std::min(Message.size(), MaximumAssetBuildDiagnosticBytes));
				return std::unexpected(std::move(Message));
			};
			if (!bAccepting || !IsValid(&Mesh) || Mesh.IsCompilationMutationBlocked()) return Reject("StaticMesh compilation is not accepting this owner.");
			if (!Request.Source.IsValid()) return Reject("StaticMesh compilation requires valid canonical source metadata.");
			const auto Snapshot = CaptureStaticMeshReconciliation(Mesh);
			const auto& InputSlots = Request.PreparedMaterialSlots ? *Request.PreparedMaterialSlots : Snapshot.MaterialSlots;
			if (!std::isfinite(Snapshot.NormalizedSize) || Snapshot.NormalizedSize <= 0
				|| InputSlots.size() > MaximumMeshMaterialSlots)
				return Reject("StaticMesh compilation settings are invalid.");
			std::unordered_set<FName> SlotNames;
			for (size_t Index = 0; Index < InputSlots.size(); ++Index)
			{
				const auto& Slot = InputSlots[Index];
				if (Slot.Name.IsNone()
	#if DURIN_WITH_EDITORONLY_DATA
					|| Slot.SourceName.size() > 4096
	#endif
					|| !SlotNames.insert(Slot.Name).second)
					return Reject(std::format("StaticMesh compilation requires bounded unique material slots (slot {}, name {}).", Index, Slot.Name.ToString()));
			}
			if (Mesh.GetAssetImportData())
			{
				if (const auto Validation = Mesh.GetAssetImportData()->Validate(); !Validation)
					return Reject(FormatAssetImportDataError(Validation.error()));
			}
			const uint64 WireBytes = Request.Source.GetGeometryBulk().GetPayloadSize();
			FAssetBuildMemoryEstimate Memory{MaximumAssetBuildRequestBytes, 1024 * 1024};
			if (!Memory.Add(WireBytes, 64) || !Memory.Add(Request.Source.GetMeshCount(), 1024)
				|| !Memory.Add(std::max<size_t>(Request.Source.GetMaterialSlotCount(), InputSlots.size()) * Request.Source.GetNumLODs(), 32768))
				return Reject(std::format("StaticMesh compilation request exceeds its {} byte budget ({} x {} bytes rejected).", Memory.Limit, Memory.RejectedCount, Memory.RejectedWidth));
			const uint64 Bytes = Memory.Bytes;
			auto Module = IMeshBuilderModule::Get();
			if (!Module) return Reject("StaticMesh compilation requires the build module.");
			const uint64 BuilderVersion = Module->GetBuildVersion();
			if (BuilderVersion == 0)
				return Reject("StaticMesh compilation requires a nonzero render builder version.");
			auto Record = std::make_shared<FRecord>();
			Record->Snapshot = Snapshot;
			Record->RequestedSource = Request.Source;
			Record->RequestedSource.ReleaseGeometry();
			Record->Package = FObjectKey(Mesh.GetPackage());
			Record->ImportData = FObjectKey(Mesh.GetAssetImportData());
			if (Mesh.GetAssetImportData()) Record->ImportState = Mesh.GetAssetImportData()->GetCompilationIdentity();
			Record->Work = std::make_shared<FWork>();
			Record->PreparedMaterialSlots = std::move(Request.PreparedMaterialSlots);
			const auto& Slots = Record->PreparedMaterialSlots ? *Record->PreparedMaterialSlots : Snapshot.MaterialSlots;
			Record->Work->Request = {.Settings = MakeStaticMeshBuildSettings(Slots, Snapshot.NormalizedSize), .Source = std::move(Request.Source)};
			Record->Work->Request.Source.ReleaseGeometry();
			Record->Work->EstimatedWorkingSetBytes = Bytes;
			Record->Work->Request.bPersistDerivedData = Request.bPersistDerivedData;
			Record->Priority = Request.Priority;
	#if DURIN_WITH_EDITOR
			Record->Work->BuildOperation = std::make_shared<DerivedData::FRequestOwner>(
				Request.Priority == EStaticMeshCompilationPriority::Interactive ? DerivedData::ERequestPriority::High : DerivedData::ERequestPriority::Low
			);
	#endif
			Record->bMarkPackageDirty = Request.bMarkPackageDirty;
			Record->bPersistDerivedData = Request.bPersistDerivedData;
			Record->Diagnostic = {.RequestId = NextRequest++, .Owner = FObjectKey(&Mesh), .EstimatedWorkingSetBytes = Bytes};
			Record->Diagnostic.SourceIdentity = Record->RequestedSource.GetIdentity();
			Record->Diagnostic.RenderBuilderVersion = BuilderVersion;
			// No invalid/rejected submission reaches this boundary or invalidates an older request.
			for (const auto& Old : Records)
				if (Old->Diagnostic.Owner == Record->Diagnostic.Owner && !Old->Terminal)
				{
					Old->bRequeue = false;
					Terminate(*Old, EStaticMeshCompilationStatus::Superseded);
				}
			Records.push_back(Record);
			Enqueue(Record);
			return {};
		}
#endif

		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			return Pump({}, Params.MaximumCompletions, Params.Deadline.value_or(FClock::time_point::max()));
		}
		auto FinishCompilationForObjects(std::span<DObject* const> Objects) -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			std::vector<FObjectKey> Selected;
			for (auto* Object : Objects)
				Selected.push_back(FObjectKey(Object));
			if (Selected.empty()) return {};
			return Finish(Selected);
		}
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
		{
			CheckOwnerThread();
			for (auto* Object : Objects)
				for (const auto& Record : Records)
					if (Record->Diagnostic.Owner == FObjectKey(Object))
					{
						Record->bRequeue = false;
						Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
					}
		}
		auto FinishAllCompilation() -> FAssetCompileProcessResult override
		{
			CheckOwnerThread();
			return Finish({});
		}
		auto Shutdown() -> void override
		{
			CheckOwnerThread();
			bAccepting = false;
			for (const auto& Record : Records)
				Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
			// Records retain their build tasks and suspended cache operations until retirement.
			FinishAllCompilation();
		}
		~FStaticMeshCompilingManager() override { Shutdown(); }

		auto Mutated(DStaticMesh& Mesh) -> void
		{
#if DURIN_WITH_EDITORONLY_DATA
			CheckOwnerThread();
			if (PublishingOwner == FObjectKey(&Mesh)) return;
			for (const auto& Record : Records)
				if (Record->Diagnostic.Owner == FObjectKey(&Mesh) && !Record->Terminal)
				{
					Record->bRequeue = !Mesh.GetRenderData() && !Record->PreparedMaterialSlots
									   && Record->RequestedSource.GetIdentity() == Record->Snapshot.SourceIdentity;
					Terminate(*Record, EStaticMeshCompilationStatus::Superseded);
				}
#else
			CheckOwnerThread();
#endif
		}

#if DURIN_WITH_EDITORONLY_DATA
		auto CanJoin(const DStaticMesh& Mesh, const FStaticMeshSource& Source) const -> bool
		{
			CheckOwnerThread();
			for (const auto& Record : Records)
				if (Record->Diagnostic.Owner == FObjectKey(const_cast<DStaticMesh*>(&Mesh))
					&& !Record->Terminal && !Record->PreparedMaterialSlots
					&& Record->RequestedSource.GetIdentity() == Source.GetIdentity() && IsCurrent(*Record, Mesh)
					&& FObjectKey(const_cast<DAssetImportData*>(Mesh.GetAssetImportData())) == Record->ImportData
					&& (!Record->ImportState || (Mesh.GetAssetImportData() && Mesh.GetAssetImportData()->GetCompilationIdentity() == *Record->ImportState)))
				{
					return true;
				}
			return false;
		}
#endif

		auto HasSourceMutation(const DStaticMesh& Mesh) const -> bool
		{
#if DURIN_WITH_EDITORONLY_DATA
			for (const auto& Record : Records)
				if (Record->Diagnostic.Owner == FObjectKey(const_cast<DStaticMesh*>(&Mesh))
					&& !Record->Terminal
					&& (Record->PreparedMaterialSlots || Record->RequestedSource.GetIdentity() != Mesh.GetSource().GetIdentity())) return true;
			return false;
#else
			return false;
#endif
		}

		auto HasPending(const DStaticMesh& Mesh) const -> bool
		{
			CheckOwnerThread();
			const auto Owner = FObjectKey(const_cast<DStaticMesh*>(&Mesh));
			return std::ranges::any_of(Records, [&](const auto& Record) { return Record->Diagnostic.Owner == Owner; });
		}
		auto Diagnostic(const DStaticMesh& Mesh) const -> FStaticMeshCompilationDiagnostic
		{
			CheckOwnerThread();
			const auto Owner = FObjectKey(const_cast<DStaticMesh*>(&Mesh));
			FStaticMeshCompilationDiagnostic Result;
			for (const auto& Record : Records)
				if (Record->Diagnostic.Owner == Owner && Record->Diagnostic.RequestId > Result.RequestId)
				{
					Result = Record->Diagnostic;
					if (!Record->Terminal && Record->Work->Phase.load(std::memory_order_acquire) == EWorkPhase::Complete)
						Result.Phase = EStaticMeshCompilationPhase::Mailbox;
				}
			for (const auto& Entry : History)
				if (Entry.Owner == Owner && Entry.RequestId > Result.RequestId) Result = Entry;
			return Result;
		}
		auto Diagnostic(uint64 RequestId) const -> FStaticMeshCompilationDiagnostic
		{
			CheckOwnerThread();
			for (const auto& Entry : History)
				if (Entry.RequestId == RequestId) return Entry;
			for (const auto& Record : Records)
				if (Record->Diagnostic.RequestId == RequestId)
				{
					auto Result = Record->Diagnostic;
					if (!Record->Terminal && Record->Work->Phase.load(std::memory_order_acquire) == EWorkPhase::Complete)
						Result.Phase = EStaticMeshCompilationPhase::Mailbox;
					return Result;
				}
			return {};
		}
		auto Diagnostics() const -> FStaticMeshCompilationManagerDiagnostics
		{
			CheckOwnerThread();
			const auto Running = std::ranges::count_if(Records, [](const auto& Record) { return Record->Work->Phase.load() == EWorkPhase::Running; });
			uint64 EstimatedBytes = 0;
			for (const auto& Record : Records) EstimatedBytes += Record->Work->EstimatedWorkingSetBytes;
			return {uint32(Records.size()), uint32(Running), static_cast<uint32>(History.size()), EstimatedBytes, bAccepting};
		}
		std::function<void(uint64, EStaticMeshCompilationPhase)> PhaseHook;

	private:
		static auto Terminate(FRecord& Record, EStaticMeshCompilationStatus Status) -> void
		{
			if (Record.Terminal) return;
			Record.Terminal = Status;
			Record.Work->Cancellation.Cancel();
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
			if (Record.Work->BuildOperation) Record.Work->BuildOperation->RequestCancel();
#endif
			if (Record.Work->Phase.load(std::memory_order_acquire) == EWorkPhase::Queued)
			{
				Record.Work->ReleaseInput();
				Record.Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
			}
		}
		auto Launch(const std::shared_ptr<FRecord>& Record, bool bDispatch, std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> FTaskHandle
		{
			Tasks::FTaskExecutionOptions Options;
			Options.Priority = Record->Priority == EStaticMeshCompilationPriority::Interactive ? ETaskPriority::High : ETaskPriority::Low;
			auto Task = Durin::FTaskHandle(Tasks::LaunchTask("StaticMesh.Build", [Slot = std::move(Slot), Work = Record->Work, Hook = PhaseHook, Id = Record->Diagnostic.RequestId, BuilderVersion = Record->Diagnostic.RenderBuilderVersion, bDispatch](const FTaskCancellationToken& Token) {
						const auto Cancellation = Work->Cancellation.WithObserver([Token] { return Token.IsCancellationRequested(); });
						try
						{
							if (bDispatch && Hook) Hook(Id, EStaticMeshCompilationPhase::Building);
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
							if (bDispatch)
							{
								Work->Phase.store(EWorkPhase::AwaitingDerivedData, std::memory_order_release);
								auto Dispatched = StaticMeshPrivate::DispatchRenderBuild(Work->Request, BuilderVersion,
									*Work->BuildOperation, Cancellation,
									Work->EstimatedWorkingSetBytes, [Work](auto Completion) {
										try { Work->Completion.emplace(std::move(Completion)); }
										catch (...) { Work->Fail("StaticMesh cache completion could not be retained."); }
										if (Work->Completion && Work->Completion->GetStatus() == DerivedData::EStatus::Canceled)
											Work->Cancellation.Cancel();
									});
								if (Dispatched)
								{
									return; // CPU slot is released; the request owner retains DDC retirement.
								}
								Work->Outcome = std::unexpected(std::move(Dispatched.error()));
							}
							else
#endif
							{
								const FAssetBuildTaskContext Control{
									.Cancellation = Cancellation,
									.MaximumWorkingSetBytes = Work->EstimatedWorkingSetBytes};
								Work->Build(Control);
							}
							if (Hook) Hook(Id, EStaticMeshCompilationPhase::Mailbox);
						}
						catch (...) { Work->Fail("Asset build worker failed with an exception."); }
						if (Cancellation.IsCancelled()) Work->ReleaseOutput();
						Work->ReleaseInput();
						Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
						}, Options));
			Record->Task = Task;
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
			if (bDispatch) Enqueue(Record, false, Task);
#endif
			return Task;
		}
		auto Enqueue(const std::shared_ptr<FRecord>& Record, bool bDispatch = true, FTaskHandle Prerequisite = {}) -> void
		{
			std::function<bool()> IsReady;
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
			if (!bDispatch) IsReady = [WeakRecord = std::weak_ptr(Record)] {
				const auto Record = WeakRecord.lock();
				return !Record || Record->Work->BuildOperation->Poll();
			};
#endif
			Scheduler->Enqueue(Record->Priority == EStaticMeshCompilationPriority::Interactive, Record->Work->EstimatedWorkingSetBytes,
				[Manager = weak_from_this(), WeakRecord = std::weak_ptr(Record), bDispatch](std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot> Slot) -> bool {
				auto Self = Manager.lock();
				auto Record = WeakRecord.lock();
				if (!Self || !Record) return {};
				if (bDispatch)
				{
					if (Record->Terminal || Record->Work->Phase.load() != EWorkPhase::Queued) return {};
					if (!IsTaskSchedulerRunning())
					{ Self->Terminate(*Record, EStaticMeshCompilationStatus::Cancelled); return {}; }
					if (Self->PhaseHook) Self->PhaseHook(Record->Diagnostic.RequestId, EStaticMeshCompilationPhase::Queued);
					if (Record->Terminal) return {};
					Record->Diagnostic.Phase = EStaticMeshCompilationPhase::Building;
				}
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
				else
				{
					if (Record->Work->Phase.load() != EWorkPhase::AwaitingDerivedData) return {};
					if (RetireDerivedDataWithoutCpu(*Record)) return false;
				}
#endif
				Record->Work->Phase.store(EWorkPhase::Running, std::memory_order_release);
				return Self->Launch(Record, bDispatch, std::move(Slot)).IsValid();
				}, std::move(Prerequisite), std::move(IsReady));
		}
#if DURIN_WITH_EDITORONLY_DATA
		static auto IsCurrent(const FRecord& Record, const DStaticMesh& Mesh) -> bool
		{
			const auto Current = CaptureStaticMeshReconciliation(Mesh);
			const auto& Expected = Record.Snapshot;
			if (Current.SourceIdentity != Expected.SourceIdentity || Current.NormalizedSize != Expected.NormalizedSize
				|| Current.MaterialSlots.size() != Expected.MaterialSlots.size()) return false;
			for (size_t Index = 0; Index < Current.MaterialSlots.size(); ++Index)
			{
				const auto& A = Current.MaterialSlots[Index];
				const auto& B = Expected.MaterialSlots[Index];
				if (A.Name != B.Name
	#if DURIN_WITH_EDITORONLY_DATA
					|| A.SourceName != B.SourceName || A.SourceMaterialIndex != B.SourceMaterialIndex
	#endif
					|| A.DefaultMaterial != B.DefaultMaterial)
					return false;
			}
			return true;
		}
#endif

		static auto Selected(const FRecord& Record, std::span<const FObjectKey> Owners) -> bool
		{
			return Owners.empty() || std::ranges::contains(Owners, Record.Diagnostic.Owner);
		}
		auto CompleteRender(const std::shared_ptr<FRecord>& Record, DStaticMesh* Mesh,
			std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>& Outcome,
			FAssetCompileProcessResult& Result) -> void
		{
#if DURIN_WITH_EDITORONLY_DATA
			if (!Mesh || FObjectKey(Mesh->GetPackage()) != Record->Package)
				Record->Terminal = EStaticMeshCompilationStatus::Cancelled;
			else if (!Outcome)
			{
				Record->Terminal = EStaticMeshCompilationStatus::Failed;
				Record->Diagnostic.Error = Outcome.error();
			}
			else
			{
				const bool bImportCurrent = FObjectKey(Mesh->GetAssetImportData()) == Record->ImportData
											&& (!Record->ImportState || (Mesh->GetAssetImportData() && Mesh->GetAssetImportData()->GetCompilationIdentity() == *Record->ImportState));
				if (!bImportCurrent)
					Record->Terminal = EStaticMeshCompilationStatus::Superseded;
				else if (!IsCurrent(*Record, *Mesh))
				{
					Record->Terminal = EStaticMeshCompilationStatus::Superseded;
					Record->bRequeue = !Record->PreparedMaterialSlots
									   && Record->RequestedSource.GetIdentity() == Record->Snapshot.SourceIdentity;
				}
				else
				{
					PublishingOwner = Record->Diagnostic.Owner;
					auto Application = CommitStaticMeshBuild(*Mesh, std::move(*Outcome), Record->RequestedSource, Record->Snapshot, Record->bMarkPackageDirty, nullptr, Record->PreparedMaterialSlots ? &*Record->PreparedMaterialSlots : nullptr, Record->bPersistDerivedData);
					const bool Applied = static_cast<bool>(Application);
					if (!Application)
					{
						Record->Diagnostic.Error = Application.error();
					}
					PublishingOwner = {};
					Record->Terminal = Applied ? EStaticMeshCompilationStatus::Succeeded : EStaticMeshCompilationStatus::Failed;
					if (Applied) Result.SuccessfullyCompiledAssets.emplace_back(Mesh);
				}
			}
#else
			Record->Terminal = EStaticMeshCompilationStatus::Failed;
#endif
		}

	private:
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
		// Producer completion closes submission; owner polling joins callbacks and native tasks.
		// Owner cancellation cleanup does not acquire CPU capacity or wait for other work.
		static auto RetireDerivedDataWithoutCpu(FRecord& Record) -> bool
		{
			auto& Work = *Record.Work;
			if (!Work.BuildOperation->Poll()) return false;
			if (!IsTaskSchedulerRunning() || Record.Task.GetState() == ETaskState::Canceled) Work.Cancellation.Cancel();
			if (!Record.Terminal && !Work.Cancellation.IsCancelled() && Work.Completion) return false;
			Work.Completion.reset();
			if (Work.Cancellation.IsCancelled()) Work.ReleaseOutput();
			Work.ReleaseInput();
			Work.Phase.store(EWorkPhase::Complete, std::memory_order_release);
			return true;
		}
#endif
		auto ProgressWork() -> void
		{
			Scheduler->Pump();
			for (const auto& Record : Records)
			{
				if (!Record->Task.IsValid() || !Record->Task.IsComplete()) continue;
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
				if (!Record->Work->BuildOperation->Poll()) continue;
#endif
				const auto Phase = Record->Work->Phase.load(std::memory_order_acquire);
				// Native tasks skipped before entry still retire through the owner mailbox.
				AssetPrivate::ObserveAssetBuildTaskCancellation(Record->Task, Record->Work->Cancellation);
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
				if (Phase == EWorkPhase::AwaitingDerivedData)
				{
					RetireDerivedDataWithoutCpu(*Record);
					continue;
				}
				else
#endif
				{
					if (Phase != EWorkPhase::Running) continue;
					if (!Record->Work->Cancellation.IsCancelled()) Record->Work->Fail("Asset build task retired before worker completion.");
				}
				Record->Work->ReleaseInput();
				Record->Work->Phase.store(EWorkPhase::Complete, std::memory_order_release);
			}
		}

	private:
		auto Pump(std::span<const FObjectKey> Owners, uint32 Maximum, FClock::time_point Deadline) -> FAssetCompileProcessResult
		{
			FAssetCompileProcessResult Result;
#if DURIN_WITH_EDITORONLY_DATA
			std::vector<std::pair<FObjectKey, FStaticMeshCompilationRequest>> Requeues;
#endif
			ProgressWork();
			const auto Pending = Records; // Callbacks may submit, cancel, or shut down this manager.
			for (const auto& Record : Pending)
			{
				if (Result.ProcessedCompletionCount >= Maximum || FClock::now() >= Deadline) break;
				if (!Selected(*Record, Owners) || !std::ranges::contains(Records, Record)) continue;
				if (Record->Work->Phase.load(std::memory_order_acquire) != EWorkPhase::Complete
					|| (Record->Task.IsValid() && !Record->Task.IsComplete())) continue;
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
				if (Record->Work->BuildOperation && !Record->Work->BuildOperation->Poll()) continue;
#endif
				// Remove before owner code can re-enter the pump. The record and
				// output stay alive until the owner callback returns, including exceptions.
				std::erase(Records, Record);
				auto Outcome = std::exchange(Record->Work->Outcome, std::unique_ptr<FStaticMeshRenderData>{});
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
				[[maybe_unused]] auto DdcCompletion = std::exchange(Record->Work->Completion, std::nullopt);
#endif
				if (!Record->Terminal && Record->Work->Cancellation.IsCancelled())
					Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
				auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Record->Diagnostic.Owner));
				if (!Record->Terminal) CompleteRender(Record, Mesh, Outcome, Result);
				if (*Record->Terminal != EStaticMeshCompilationStatus::Failed) Record->Diagnostic.Error.clear();
				Record->Diagnostic.Error.resize(std::min(Record->Diagnostic.Error.size(), MaximumAssetBuildDiagnosticBytes));
				Record->Diagnostic.Status = *Record->Terminal;
				Record->Diagnostic.Phase = EStaticMeshCompilationPhase::Terminal;
				History.push_back(Record->Diagnostic);
				if (History.size() > MaximumHistory) History.pop_front();
#if DURIN_WITH_EDITORONLY_DATA
				if (Record->bRequeue && Mesh && bAccepting)
					Requeues.emplace_back(Record->Diagnostic.Owner, FStaticMeshCompilationRequest{.Source = Mesh->GetSource().GetIdentity() == Record->Snapshot.SourceIdentity ? Record->RequestedSource : Mesh->GetSource(), .Priority = Record->Priority, .bMarkPackageDirty = Record->bMarkPackageDirty});
				Record->RequestedSource = {};
#endif

				Record->Snapshot = {};
				Record->PreparedMaterialSlots.reset();
				Record->ImportState.reset();
				++Result.ProcessedCompletionCount;
			}
#if DURIN_WITH_EDITORONLY_DATA
			for (auto& [Owner, Request] : Requeues)
				if (auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Owner)); Mesh && !HasPending(*Mesh))
				{
					static_cast<void>(Submit(*Mesh, std::move(Request)));
				}
#endif

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
				Result.SuccessfullyCompiledAssets.insert(Result.SuccessfullyCompiledAssets.end(), Batch.SuccessfullyCompiledAssets.begin(), Batch.SuccessfullyCompiledAssets.end());
				if (Batch.ProcessedCompletionCount == 0)
				{
					bool JoinedOwner = false;
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
					for (const auto& Record : Records)
						if (Selected(*Record, Owners) && Record->Work->BuildOperation
							&& AssetPrivate::WaitForAssetBuildOwner(Record->Task, *Record->Work->BuildOperation))
						{ JoinedOwner = true; break; }
#endif
					if (!JoinedOwner) Scheduler->WaitForProgress();
				}
			}
			return Result;
		}

		std::vector<std::shared_ptr<FRecord>> Records;
		std::deque<FStaticMeshCompilationDiagnostic> History;
		std::shared_ptr<AssetPrivate::FAssetBuildScheduler> Scheduler = AssetPrivate::GetAssetBuildScheduler();
		FObjectKey PublishingOwner;
		uint64 NextRequest = 1;
		bool bAccepting = false;
	};
	namespace
	{
		std::weak_ptr<FStaticMeshCompilingManager> GManager;
	}

	namespace AssetPrivate
	{
		auto CreateStaticMeshCompilingManager() -> std::shared_ptr<IAssetCompilingManager>
		{
			CheckOwnerThread();
			if (auto Existing = GManager.lock()) return Existing;
			auto Manager = std::make_shared<FStaticMeshCompilingManager>();
			GManager = Manager;
			return Manager;
		}
		auto SetStaticMeshCompilationPhaseHookForTests(std::function<void(uint64, EStaticMeshCompilationPhase)> Hook) -> void
		{
			CheckOwnerThread();
			if (auto Manager = GManager.lock()) Manager->PhaseHook = std::move(Hook);
		}
	} // namespace AssetPrivate
	auto FormatStaticMeshCompilationDiagnostic(const FStaticMeshCompilationDiagnostic& Diagnostic) -> std::string
	{
		auto Message = Diagnostic.Error;
		Message.resize(std::min(Message.size(), MaximumAssetBuildDiagnosticBytes));
		return Message;
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto BuildStaticMeshSourceSynchronously(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request)
		-> std::expected<void, std::string>;

	auto DStaticMesh::BuildFromSource(EStaticMeshBuildMode Mode, FStaticMeshCompilationRequest Request) -> std::expected<void, std::string>
	{
		CheckOwnerThread();
		if (AssetPrivate::IsStaticMeshCompilationMutationBlocked(*this))
			return std::unexpected(std::string("StaticMesh mutation is retiring prior compilation."));
		if (Mode == EStaticMeshBuildMode::Synchronous)
		{
			return BuildStaticMeshSourceSynchronously(*this, std::move(Request));
		}
		if (Mode != EStaticMeshBuildMode::Asynchronous)
			return std::unexpected(std::string("Invalid StaticMesh build mode."));
		if (auto Manager = GManager.lock()) return Manager->Submit(*this, std::move(Request));
		return std::unexpected(std::string("The StaticMesh compiling manager is unavailable."));
	}
#endif
#if DURIN_WITH_EDITORONLY_DATA
	auto CanJoinStaticMeshCompilation(const DStaticMesh& Mesh, const FStaticMeshSource& Source) -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->CanJoin(Mesh, Source);
	}
#endif
	auto HasPendingStaticMeshSourceMutation(const DStaticMesh& Mesh) -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->HasSourceMutation(Mesh);
	}
	auto DStaticMesh::IsCompiling() const -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->HasPending(*this);
	}
	auto DStaticMesh::GetCompilationDiagnostic() const -> FStaticMeshCompilationDiagnostic
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager ? Manager->Diagnostic(*this) : FStaticMeshCompilationDiagnostic{};
	}
	auto GetStaticMeshCompilationDiagnostic(uint64 RequestId) -> FStaticMeshCompilationDiagnostic
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager ? Manager->Diagnostic(RequestId) : FStaticMeshCompilationDiagnostic{};
	}
	auto GetStaticMeshCompilationManagerDiagnostics() -> FStaticMeshCompilationManagerDiagnostics
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager ? Manager->Diagnostics() : FStaticMeshCompilationManagerDiagnostics{};
	}
	auto DStaticMesh::FinishCompilation() -> FAssetCompileProcessResult
	{
		CheckOwnerThread();
		return FAssetCompilingManager::Get().FinishCompilationForObject(*this);
	}

	auto DStaticMesh::CancelCompilation() -> void
	{
		CheckOwnerThread();
		DObject* Object = this;
		if (auto Manager = GManager.lock()) Manager->MarkCompilationAsCanceled({&Object, 1});
	}
	auto NotifyStaticMeshCompilationMutation(DStaticMesh& Mesh) -> void
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) Manager->Mutated(Mesh);
	}
} // namespace Durin

#include "Threading/TaskComposition.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Asset/AssetReadResult.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCookHelper.h"
#include "Physics/PhysicsMeshCompilation.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

#include <deque>
#include <cmath>
#include <condition_variable>
#include <variant>

namespace Durin
{
	namespace
	{
		using FClock = std::chrono::steady_clock;
		constexpr uint64 MaximumRequestBytes = 512ull * 1024 * 1024;
		constexpr uint64 MaximumTotalBytes = 1024ull * 1024 * 1024;
		constexpr size_t MaximumRecords = 32;
		constexpr size_t MaximumHistory = 128;

		auto CheckOwnerThread() -> void { if (GIsGameThreadIdInitialized) CheckGameThread(); }

		struct FRenderWork
		{
			uint64 ProviderRegistration = 0;
			FStaticMeshBuildRequest Request;
			std::vector<FStaticMeshCacheError> CacheErrors;
			std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure> Outcome =
				std::unexpected(FStaticMeshBuildFailure{"StaticMesh render build has not started."});
			auto Build(const FAssetBuildTaskContext& Control) -> void
			{ Outcome = FStaticMeshBuilder::Build(std::move(Request), Control, &CacheErrors, ProviderRegistration); }
		};
		struct FCollisionWork
		{
			struct FRequest
			{
				std::unique_ptr<FPhysicsMeshInputTask> InputTask;
				FCookBodySetupInfo CookInfo;
			} Request;
			std::expected<FPhysicsCookResult, FPhysicsCookFailure> Outcome =
				std::unexpected(FPhysicsCookFailure{"Physics cook has not started."});
			auto Build(const FAssetBuildTaskContext& Control) -> void
			{
				auto Data = Request.InputTask->Execute(Control);
				if (!Data) { Outcome = std::unexpected(Data.error()); return; }
				Request.CookInfo.TriangleMeshDesc = std::move(*Data);
				Outcome = FPhysicsCookHelper::Cook(Request.CookInfo, Control);
			}
		};
		// The selected payload type never changes while the worker/owner share this capture.
		// It contains detached inputs and no callbacks to owners.
		struct FWork
		{
			std::variant<FRenderWork, FCollisionWork> Payload;
			uint64 ReservedBytes = 0;
			std::atomic<bool> Cancelled = false;
			std::atomic<bool> Done = false;
			FWork() = default;
			explicit FWork(std::in_place_type_t<FCollisionWork>) : Payload(std::in_place_type<FCollisionWork>) {}
			auto IsCollision() const -> bool { return std::holds_alternative<FCollisionWork>(Payload); }
			auto Render() -> FRenderWork& { return std::get<FRenderWork>(Payload); }
			auto Collision() -> FCollisionWork& { return std::get<FCollisionWork>(Payload); }
			auto Build(const FAssetBuildTaskContext& Control) -> void
			{ std::visit([&](auto& Work) { Work.Build(Control); }, Payload); }
			auto Fail(std::string Message) -> void
			{
				std::visit([&](auto& Work) {
					using FFailure = typename decltype(Work.Outcome)::error_type;
					Work.Outcome = std::unexpected(FFailure(Message));
				}, Payload);
			}
			auto ReleaseInput() -> void
			{ std::visit([](auto& Work) { Work.Request = {}; }, Payload); }
			auto ReleaseOutput() -> void
			{ std::visit([](auto& Work) { Work.Outcome = typename decltype(Work.Outcome)::value_type{}; }, Payload); }
		};
		// Collision publication facts are private scheduler state, separate from render reconciliation.
		struct FCollisionOwnerSnapshot
		{
			FObjectKey Body;
			uint64 BodyRevision = 0;
			EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
			EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
			uint64 RequestGeneration = 0;
			static auto Capture(DBodySetup* Body) -> FCollisionOwnerSnapshot
			{
				return {.Body = FObjectKey(Body), .BodyRevision = Body ? Body->GetRevision() : 0,
					.CollisionMode = Body ? Body->GetCollisionSourceMode() : EBodySetupCollisionSourceMode::None,
					.CollisionPolicy = Body ? Body->GetCollisionQueryPolicy() : EBodySetupCollisionQueryPolicy::SimpleAndComplex,
					.RequestGeneration = Body ? Body->GetPhysicsMeshRequestGeneration() : 0};
			}
		};
		struct FWorkerState
		{
			std::atomic<uint32> Running = 0;
			std::mutex Mutex;
			std::condition_variable Changed;
		};
		struct FRecord
		{
			FStaticMeshCompilationDiagnostic Diagnostic;
			std::variant<FStaticMeshReconciliationSnapshot, FCollisionOwnerSnapshot> Snapshot;
			auto RenderSnapshot() const -> const FStaticMeshReconciliationSnapshot& { return std::get<FStaticMeshReconciliationSnapshot>(Snapshot); }
			auto CollisionSnapshot() const -> const FCollisionOwnerSnapshot& { return std::get<FCollisionOwnerSnapshot>(Snapshot); }
			std::optional<std::vector<FMeshMaterialSlotDefinition>> PreparedMaterialSlots;
			FObjectKey AssetOwner;
			FObjectKey Package;
			FObjectKey ImportData;
			FStaticMeshSource RequestedSource;
			bool bRequeue = false;
			std::optional<FXxHash128> ImportState;
			FStaticMeshBuildProviderDescriptor Descriptor;
			FStaticMeshCompilationCompletion Completion;
			FOnAsyncPhysicsCookFinished PhysicsCompletion;
			FStaticMeshPublicationPreparation PreparePublication;
			std::shared_ptr<FWork> Work;
			FTaskHandle Task;
			EStaticMeshCompilationPriority Priority;
			std::optional<EStaticMeshCompilationStatus> Terminal;
			bool bMarkPackageDirty = true;
			bool bPersistDerivedData = true;
			bool bStarted = false;
			bool bDelivered = false;
		};

	}

	class FStaticMeshCompilingManager final : public IAssetCompilingManager
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
				bShutdown = false;
				InteractiveBurst = 0;
				LastPumpIdentity = 0;
				return {bAccepting ? EAssetCompilerStartError::None : EAssetCompilerStartError::TaskScopeUnavailable};
			}
			auto StopAdmission() -> void override
			{
				CheckOwnerThread();
				bAccepting = false;
				for (const auto& Record : Records) Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
			}
			auto GetNumRemainingAssets() const -> uint64 override { CheckOwnerThread(); return Records.size(); }

			auto Submit(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request,
				FStaticMeshCompilationCompletion Completion) -> std::expected<void, std::vector<std::string>>
			{
				CheckOwnerThread();
				const auto Reject = [](std::string Message) -> std::expected<void, std::vector<std::string>> {
					Message.resize(std::min(Message.size(), MaximumStaticMeshBuildDiagnosticBytes));
					return std::unexpected(std::vector<std::string>{std::move(Message)});
				};
				if (!bAccepting || !IsValid(&Mesh)) return Reject("StaticMesh compilation is not accepting this owner.");
				if (!Request.Source.IsValid()) return Reject("StaticMesh compilation requires valid canonical source metadata.");
				const auto Snapshot = FStaticMeshBuilder::Capture(Mesh);
				const auto& InputSlots = Request.PreparedMaterialSlots ? *Request.PreparedMaterialSlots : Snapshot.MaterialSlots;
				if (!std::isfinite(Snapshot.NormalizedSize) || Snapshot.NormalizedSize <= 0
					|| InputSlots.size() > MaximumMeshMaterialSlots)
					return Reject("StaticMesh compilation settings are invalid.");
				std::unordered_set<FName> SlotNames;
				for (size_t Index = 0; Index < InputSlots.size(); ++Index)
				{
					const auto& Slot = InputSlots[Index];
					if (Slot.Name.IsNone() || Slot.SourceName.size() > 4096 || !SlotNames.insert(Slot.Name).second)
						return Reject(std::format("StaticMesh compilation requires bounded unique material slots (slot {}, name {}).", Index, Slot.Name.ToString()));
				}
				if (Mesh.GetAssetImportData())
				{
					if (const auto Validation = Mesh.GetAssetImportData()->Validate(); !Validation)
						return Reject(FormatAssetImportDataError(Validation.error()));
				}
				const uint64 WireBytes = Request.Source.GetGeometryBulk().GetPayloadSize();
				FAssetBuildMemoryEstimate Memory{MaximumRequestBytes, 1024 * 1024};
				if (!Memory.Add(WireBytes, 64) || !Memory.Add(Request.Source.GetMeshCount(), 1024)
					|| !Memory.Add(std::max<size_t>(Request.Source.GetMaterialSlotCount(), InputSlots.size()), 32768))
					return Reject(std::format("StaticMesh compilation request exceeds its {} byte budget ({} x {} bytes rejected).", Memory.Limit, Memory.RejectedCount, Memory.RejectedWidth));
				const uint64 Bytes = Memory.Bytes;
				if (Records.size() >= MaximumRecords || Bytes > MaximumTotalBytes - ReservedBytes)
					return Reject(std::format("StaticMesh compilation admission budget exhausted ({} / {} records, {} reserved bytes, {} requested bytes).", Records.size(), MaximumRecords, ReservedBytes, Bytes));
				const auto Provider = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
					[](IStaticMeshBuildProvider& Value) { return Value.GetDescriptor(); });
				if (!Provider.WasInvoked() || !Provider.Value || !Provider.Value->IsValid()
					|| Provider.Value->ProducerIdentity.size() > 256)
					return Reject("StaticMesh compilation requires one valid build provider.");
				auto Record = std::make_shared<FRecord>();
				Record->Snapshot = Snapshot;
				Record->RequestedSource = Request.Source;
				Record->RequestedSource.ReleaseGeometry();
				Record->Package = FObjectKey(Mesh.GetPackage());
				Record->ImportData = FObjectKey(Mesh.GetAssetImportData());
				if (Mesh.GetAssetImportData()) Record->ImportState = Mesh.GetAssetImportData()->GetCompilationIdentity();
				Record->Descriptor = *Provider.Value;
				Record->Completion = std::move(Completion);
				Record->PreparePublication = std::move(Request.PreparePublication);
				Record->Work = std::make_shared<FWork>();
				auto Input = Snapshot;
				Record->PreparedMaterialSlots = std::move(Request.PreparedMaterialSlots);
				if (Record->PreparedMaterialSlots) Input.MaterialSlots = *Record->PreparedMaterialSlots;
				// Workers retain only value metadata; material object bindings stay in Snapshot.
				for (auto& Slot : Input.MaterialSlots) Slot.DefaultMaterial = nullptr;
				Record->Work->Render().Request = {.Reconciliation = std::move(Input), .Source = std::move(Request.Source)};
				Record->Work->Render().Request.Source.ReleaseGeometry();
				Record->Work->ReservedBytes = Bytes;
				Record->Work->Render().Request.bPersistDerivedData = Request.bPersistDerivedData;
				Record->Work->Render().ProviderRegistration = Provider.RegistrationIdentity;
				Record->Priority = Request.Priority;
				Record->bMarkPackageDirty = Request.bMarkPackageDirty;
				Record->bPersistDerivedData = Request.bPersistDerivedData;
				Record->Diagnostic = {.RequestId = NextRequest++, .Owner = FObjectKey(&Mesh), .ReservedBytes = Bytes};
				Record->Diagnostic.SourceIdentity = Record->RequestedSource.GetIdentity();
				Record->Diagnostic.Descriptor = Record->Descriptor;
				Record->Diagnostic.ProviderRegistration = Provider.RegistrationIdentity;
				// No invalid/rejected submission reaches this boundary or invalidates an older request.
				for (const auto& Old : Records)
					if (!Old->Work->IsCollision() && Old->Diagnostic.Owner == Record->Diagnostic.Owner && !Old->bDelivered)
						{ Old->bRequeue = false; Terminate(*Old, EStaticMeshCompilationStatus::Superseded); }
				ReservedBytes += Bytes;
				Records.push_back(std::move(Record));
				Admit();
				return {};
			}

			auto CancelCollision(DBodySetup& Body) -> void
			{
				CheckOwnerThread();
				for (const auto& Record : Records)
					if (Record->Work->IsCollision() && Record->Diagnostic.Owner == FObjectKey(&Body))
						Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
			}
			auto SubmitCollision(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask, bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion)
				-> std::expected<void, FPhysicsCookFailure>
			{
				CheckOwnerThread();
				const auto Reject = [](std::string Message) -> std::expected<void, FPhysicsCookFailure> {
					return std::unexpected(FPhysicsCookFailure{std::move(Message), EPhysicsCookStage::Scheduling});
				};
				if (!bAccepting || !IsValid(&Body)) return Reject("Physics mesh compilation is not accepting this owner.");
				if (!InputTask || InputTask->GetWorkingSetBytes() == 0 || InputTask->GetWorkingSetBytes() > MaximumRequestBytes)
					return Reject("Physics mesh preparation requires a bounded detached task.");
				if (Records.size() >= MaximumRecords || InputTask->GetWorkingSetBytes() > MaximumTotalBytes - ReservedBytes)
					return Reject("Physics mesh compilation admission budget exhausted.");
				auto Record = std::make_shared<FRecord>();
				Record->Snapshot = FCollisionOwnerSnapshot::Capture(&Body);
				Record->AssetOwner = FObjectKey(Body.GetOuter());
				Record->PhysicsCompletion = std::move(Completion);
				Record->Priority = EStaticMeshCompilationPriority::Background;
				Record->Work = std::make_shared<FWork>(std::in_place_type<FCollisionWork>);
				Record->Work->ReservedBytes = InputTask->GetWorkingSetBytes();
				Record->Work->Collision().Request = {std::move(InputTask), Body.GetCookInfo({}, bPersistDerivedData)};
				Record->Diagnostic = {.RequestId = NextRequest++, .Owner = FObjectKey(&Body), .ReservedBytes = Record->Work->ReservedBytes};
				ReservedBytes += Record->Work->ReservedBytes;
				Records.push_back(std::move(Record));
				Admit();
				return {};
			}

			auto FinishForMesh(DStaticMesh& Mesh, EStaticMeshCompilationProducts Products) -> FAssetCompileProcessResult
			{
				const FObjectKey Owner(&Mesh);
				return Finish({&Owner, 1}, Products);
			}
			auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult override
			{
				CheckOwnerThread();
				if (Params.PumpIdentity == 0 || Params.PumpIdentity != LastPumpIdentity)
				{
					LastPumpIdentity = Params.PumpIdentity;
					PumpCount = 0;
					PumpDeadline = FClock::now() + std::chrono::milliseconds(2);
				}
				const auto Deadline = std::min(Params.Deadline.value_or(FClock::time_point::max()), PumpDeadline);
				auto Result = Pump({}, std::min<uint32>(2 - PumpCount, Params.MaximumCompletions), Deadline);
				PumpCount += Result.ProcessedCompletionCount;
				return Result;
			}
			auto FinishCompilationForObjects(std::span<DObject* const> Objects) -> FAssetCompileProcessResult override
			{
				CheckOwnerThread();
				std::vector<FObjectKey> Selected;
				for (auto* Object : Objects) Selected.push_back(FObjectKey(Object));
				if (Selected.empty()) return {};
				return Finish(Selected);
			}
			auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
			{
				CheckOwnerThread();
				for (auto* Object : Objects)
					for (const auto& Record : Records)
						if (Record->Diagnostic.Owner == FObjectKey(Object) || Record->AssetOwner == FObjectKey(Object))
						{ Record->bRequeue = false; Terminate(*Record, EStaticMeshCompilationStatus::Cancelled); }
			}
			auto FinishAllCompilation() -> FAssetCompileProcessResult override { CheckOwnerThread(); return Finish({}); }
			auto Shutdown() -> void override
			{
				CheckOwnerThread();
				bAccepting = false;
				bShutdown = true;
				for (const auto& Record : Records) Terminate(*Record, EStaticMeshCompilationStatus::Cancelled);
				// Cooperative flags cancel recipes. Drain the scope so every launched body publishes its mailbox.
				if (Scope.IsValid()) { Scope.Close(ETaskScopeCloseMode::Drain); Scope.Wait(); }
				FinishAllCompilation();
			}
			~FStaticMeshCompilingManager() override { Shutdown(); }

			auto Mutated(DStaticMesh& Mesh) -> void
			{
				CheckOwnerThread();
				if (PublishingOwner == FObjectKey(&Mesh)) return;
				for (const auto& Record : Records)
					if (!Record->Work->IsCollision() && Record->Diagnostic.Owner == FObjectKey(&Mesh) && !Record->Terminal && !Record->bDelivered)
					{
						Record->bRequeue = !Mesh.GetRenderData() && !Record->PreparePublication && !Record->PreparedMaterialSlots
							&& Record->RequestedSource.GetIdentity() == Record->RenderSnapshot().SourceIdentity;
						Terminate(*Record, EStaticMeshCompilationStatus::Superseded);
					}
			}

			auto CanJoin(const DStaticMesh& Mesh, const FStaticMeshSource& Source) const -> bool
			{
				CheckOwnerThread();
				for (const auto& Record : Records)
					if (!Record->Work->IsCollision() && Record->Diagnostic.Owner == FObjectKey(const_cast<DStaticMesh*>(&Mesh))
						&& !Record->bDelivered && !Record->Terminal && !Record->PreparePublication && !Record->PreparedMaterialSlots
						&& Record->RequestedSource.GetIdentity() == Source.GetIdentity() && IsCurrent(*Record, Mesh)
						&& FObjectKey(const_cast<DAssetImportData*>(Mesh.GetAssetImportData())) == Record->ImportData
						&& (!Record->ImportState || (Mesh.GetAssetImportData()
							&& Mesh.GetAssetImportData()->GetCompilationIdentity() == *Record->ImportState)))
					{
						const auto Provider = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
							[&](IStaticMeshBuildProvider& Value) {
								const auto Current = Value.GetDescriptor();
								return Current.ProducerIdentity == Record->Descriptor.ProducerIdentity
									&& Current.RenderBuilderVersion == Record->Descriptor.RenderBuilderVersion;
							}, Record->Work->Render().ProviderRegistration);
						return Provider.WasInvoked() && Provider.Value.value_or(false);
					}
				return false;
			}

			auto HasSourceMutation(const DStaticMesh& Mesh) const -> bool
			{
				for (const auto& Record : Records)
					if (!Record->Work->IsCollision() && Record->Diagnostic.Owner == FObjectKey(const_cast<DStaticMesh*>(&Mesh))
						&& !Record->bDelivered && !Record->Terminal
						&& (Record->PreparePublication || Record->PreparedMaterialSlots || Record->RequestedSource.GetIdentity() != Mesh.GetSource().GetIdentity())) return true;
				return false;
			}

			auto HasPending(const DStaticMesh& Mesh) const -> bool
			{
				CheckOwnerThread();
				const auto Owner = FObjectKey(const_cast<DStaticMesh*>(&Mesh));
				return std::ranges::any_of(Records, [&](const auto& Record) { return !Record->Work->IsCollision() && Record->Diagnostic.Owner == Owner && !Record->bDelivered; });
			}
			auto Diagnostic(const DStaticMesh& Mesh) const -> FStaticMeshCompilationDiagnostic
			{
				CheckOwnerThread();
				const auto Owner = FObjectKey(const_cast<DStaticMesh*>(&Mesh));
				FStaticMeshCompilationDiagnostic Result;
				for (const auto& Record : Records)
					if (!Record->Work->IsCollision() && Record->Diagnostic.Owner == Owner && Record->Diagnostic.RequestId > Result.RequestId)
					{
						Result = Record->Diagnostic;
						if (!Record->Terminal && Record->Work->Done.load(std::memory_order_acquire))
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
						if (!Record->Terminal && Record->Work->Done.load(std::memory_order_acquire))
							Result.Phase = EStaticMeshCompilationPhase::Mailbox;
						return Result;
					}
				return {};
			}
			auto Diagnostics() const -> FStaticMeshCompilationManagerDiagnostics
			{
				CheckOwnerThread();
				return {static_cast<uint32>(Records.size()), Workers->Running.load(),
					static_cast<uint32>(History.size()), ReservedBytes, bAccepting};
			}
			std::function<void(uint64, EStaticMeshCompilationPhase)> PhaseHook;

		private:
			static auto Terminate(FRecord& Record, EStaticMeshCompilationStatus Status) -> void
			{
				if (Record.bDelivered || Record.Terminal) return;
				Record.Terminal = Status;
				if (Record.Work->IsCollision())
					if (auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record.Diagnostic.Owner)))
						Body->CancelPhysicsMeshes(Record.CollisionSnapshot().RequestGeneration);
				Record.Work->Cancelled.store(true, std::memory_order_release);
				if (!Record.bStarted)
				{
					Record.Work->ReleaseInput();
					Record.Work->Done.store(true, std::memory_order_release);
				}
			}
			auto Admit() -> void
			{
				while (!bShutdown && Workers->Running.load() < 2)
				{
					std::shared_ptr<FRecord> Background, Interactive;
					for (const auto& Record : Records)
					{
						if (Record->bStarted || Record->Terminal) continue;
						auto& First = Record->Priority == EStaticMeshCompilationPriority::Interactive ? Interactive : Background;
						if (!First) First = Record;
					}
					auto Record = Interactive && (!Background || InteractiveBurst < 4) ? Interactive : Background;
					if (!Record) return;
					if (Record == Interactive) ++InteractiveBurst; else InteractiveBurst = 0;
					if (PhaseHook) PhaseHook(Record->Diagnostic.RequestId, EStaticMeshCompilationPhase::Queued);
					Record->bStarted = true;
					Record->Diagnostic.Phase = EStaticMeshCompilationPhase::Building;
					Workers->Running.fetch_add(1);
					Tasks::FTaskExecutionOptions Options;
					Options.Scope = Scope.GetToken();
					Record->Task = Tasks::LaunchTask("StaticMesh.Build",
						[Work = Record->Work, State = Workers, Hook = PhaseHook, Id = Record->Diagnostic.RequestId](const FTaskCancellationToken& Token) {
							try
							{
								if (Hook) Hook(Id, EStaticMeshCompilationPhase::Building);
								const FAssetBuildTaskContext Control{
									.ShouldCancel = [&] { return Work->Cancelled.load(std::memory_order_acquire) || Token.IsCancellationRequested(); },
									.MaximumWorkingSetBytes = Work->ReservedBytes};
								Work->Build(Control);
								if (Hook) Hook(Id, EStaticMeshCompilationPhase::Mailbox);
							}
							catch (...) {
								Work->Fail("Asset build worker failed with an exception.");
							}
							Work->ReleaseInput();
							Work->Done.store(true, std::memory_order_release);
							State->Running.fetch_sub(1);
							State->Changed.notify_all();
						}, Options).GetCompletion().GetTaskHandle();

				}
			}
			static auto IsCurrent(const FRecord& Record, const DStaticMesh& Mesh) -> bool
			{
				const auto Current = FStaticMeshBuilder::Capture(Mesh);
				const auto& Expected = Record.RenderSnapshot();
				if (Current.SourceIdentity != Expected.SourceIdentity || Current.NormalizedSize != Expected.NormalizedSize
					|| Current.MaterialSlots.size() != Expected.MaterialSlots.size()) return false;
				for (size_t Index = 0; Index < Current.MaterialSlots.size(); ++Index)
				{
					const auto& A = Current.MaterialSlots[Index];
					const auto& B = Expected.MaterialSlots[Index];
					if (A.Name != B.Name || A.SourceName != B.SourceName || A.SourceMaterialIndex != B.SourceMaterialIndex
						|| A.DefaultMaterial != B.DefaultMaterial) return false;
				}
				return true;
			}

			static auto Selected(const FRecord& Record, std::span<const FObjectKey> Owners, EStaticMeshCompilationProducts Products = EStaticMeshCompilationProducts::All) -> bool
			{
				if (Products == EStaticMeshCompilationProducts::Render && Record.Work->IsCollision()) return false;
				if (Products == EStaticMeshCompilationProducts::Collision && !Record.Work->IsCollision()) return false;
				return Owners.empty() || std::ranges::contains(Owners, Record.Diagnostic.Owner)
					|| std::ranges::contains(Owners, Record.AssetOwner);
			}
			auto CompleteCollision(const std::shared_ptr<FRecord>& Record, DBodySetup* Body) -> void
			{
				const auto& Expected = Record->CollisionSnapshot();
				const auto Current = FCollisionOwnerSnapshot::Capture(Body);
				if (!Body || Current.RequestGeneration != Expected.RequestGeneration
					|| Current.Body != Expected.Body || Current.BodyRevision != Expected.BodyRevision
					|| Current.CollisionMode != Expected.CollisionMode || Current.CollisionPolicy != Expected.CollisionPolicy)
				{
					if (Body) Body->CancelPhysicsMeshes(Expected.RequestGeneration);
					Record->Terminal = EStaticMeshCompilationStatus::Superseded;
				}
				else if (!Record->Work->Collision().Outcome)
				{
					const auto& Error = Record->Work->Collision().Outcome.error();
					Body->FailPhysicsMeshes(Expected.RequestGeneration, Error);
					Record->Terminal = Error.IsCancelled() ? EStaticMeshCompilationStatus::Cancelled : EStaticMeshCompilationStatus::Failed;
				}
				else
				{
					const auto& Collision = *Record->Work->Collision().Outcome;
					const bool Installed = Body->ApplyPhysicsMeshes(Expected.RequestGeneration, Collision.Simple, Collision.Complex);
					Record->Terminal = Installed ? EStaticMeshCompilationStatus::Succeeded : EStaticMeshCompilationStatus::Failed;
					for (const auto& Warning : Collision.GetCacheErrors()) DURIN_WARN("{}", Warning.ToString());
				}
			}

			auto CompleteRender(const std::shared_ptr<FRecord>& Record, DStaticMesh* Mesh,
				FAssetCompileProcessResult& Result) -> void
			{
				if (!Record->Work->Render().Outcome) Record->Diagnostic.Error = Record->Work->Render().Outcome.error();
				if (Record->Work->Render().Outcome)
				{
					Record->Diagnostic.CacheErrors = Record->Work->Render().CacheErrors;
				}
				if (!Mesh || FObjectKey(Mesh->GetPackage()) != Record->Package)
					Record->Terminal = EStaticMeshCompilationStatus::Cancelled;
				else if (!Record->Work->Render().Outcome)
					Record->Terminal = Record->Work->Render().Outcome.error().IsCancelled()
						? EStaticMeshCompilationStatus::Cancelled : EStaticMeshCompilationStatus::Failed;
				else
				{
					const auto Provider = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
						[&](IStaticMeshBuildProvider& Value) {
							const auto Current = Value.GetDescriptor();
							return Current.ProducerIdentity == Record->Descriptor.ProducerIdentity
								&& Current.RenderBuilderVersion == Record->Descriptor.RenderBuilderVersion;
						}, Record->Work->Render().ProviderRegistration);
					const bool bImportCurrent = FObjectKey(Mesh->GetAssetImportData()) == Record->ImportData
						&& (!Record->ImportState || (Mesh->GetAssetImportData() && Mesh->GetAssetImportData()->GetCompilationIdentity() == *Record->ImportState));
					if (!Provider.WasInvoked() || !Provider.Value.value_or(false) || !bImportCurrent)
						Record->Terminal = EStaticMeshCompilationStatus::Superseded;
					else if (!IsCurrent(*Record, *Mesh))
					{
						Record->Terminal = EStaticMeshCompilationStatus::Superseded;
						Record->bRequeue = !Record->PreparePublication && !Record->PreparedMaterialSlots
							&& Record->RequestedSource.GetIdentity() == Record->RenderSnapshot().SourceIdentity;
					}
					else
					{
						DAssetImportData* PreparedImportData = nullptr;
						std::expected<void, FStaticMeshBuildFailure> Application = Record->PreparePublication
							? Record->PreparePublication(*Mesh, PreparedImportData) : std::expected<void, FStaticMeshBuildFailure>{};
						PublishingOwner = Record->Diagnostic.Owner;
						if (Application)
							Application = CommitStaticMeshBuild(*Mesh, std::move(*Record->Work->Render().Outcome), Record->RequestedSource,
								Record->RenderSnapshot(), Record->bMarkPackageDirty, {}, PreparedImportData,
								Record->PreparedMaterialSlots ? &*Record->PreparedMaterialSlots : nullptr, Record->bPersistDerivedData);
						const bool Applied = static_cast<bool>(Application);
						if (!Application)
						{
							Record->Diagnostic.Error = Application.error();
						}
						PublishingOwner = {};
						Record->Terminal = Applied ? EStaticMeshCompilationStatus::Succeeded
							: Application.error().IsCancelled() ? EStaticMeshCompilationStatus::Cancelled : EStaticMeshCompilationStatus::Failed;
						if (Applied) Result.SuccessfullyCompiledAssets.emplace_back(Mesh);
					}
				}
			}

			auto Pump(std::span<const FObjectKey> Owners, uint32 Maximum, FClock::time_point Deadline, EStaticMeshCompilationProducts Products = EStaticMeshCompilationProducts::All) -> FAssetCompileProcessResult
			{
				FAssetCompileProcessResult Result;
				std::vector<std::pair<FObjectKey, FStaticMeshCompilationRequest>> Requeues;
				// Scheduler cancellation may retire a task without entering its body.
				for (const auto& Record : Records)
				{
					if (!Record->Task.IsValid() || !Record->Task.IsComplete()
						|| Record->Work->Done.load(std::memory_order_acquire)) continue;
					Record->Work->Fail("Asset build task retired before worker completion.");
					Record->Work->ReleaseInput();
					Record->Work->Done.store(true, std::memory_order_release);
					Workers->Running.fetch_sub(1);
					Workers->Changed.notify_all();
				}
				Admit();
				const auto Pending = Records; // Callbacks may submit, cancel, or shut down this manager.
				for (const auto& Record : Pending)
				{
					if (Result.ProcessedCompletionCount >= Maximum || FClock::now() >= Deadline) break;
					if (!Selected(*Record, Owners, Products) || Record->bDelivered) continue;
					if (!Record->Terminal && (!Record->Work->Done.load(std::memory_order_acquire)
						|| (Record->Task.IsValid() && !Record->Task.IsComplete()))) continue;
					// Replacements reuse the retiring record's count/byte capacity. Wait for its
					// late worker before releasing that charge, even when the queue is saturated.
					if (Record->bRequeue && (!Record->Work->Done.load(std::memory_order_acquire)
						|| (Record->Task.IsValid() && !Record->Task.IsComplete()))) continue;
					auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Record->Diagnostic.Owner));
					if (!Record->Terminal)
					{
						if (Record->Work->IsCollision()) CompleteCollision(Record, Cast<DBodySetup>(ResolveObjectKey(Record->Diagnostic.Owner)));
						else CompleteRender(Record, Mesh, Result);
					}
					if (Record->Work->IsCollision() && *Record->Terminal != EStaticMeshCompilationStatus::Succeeded)
						if (auto* Body = Cast<DBodySetup>(ResolveObjectKey(Record->Diagnostic.Owner)))
							Body->CancelPhysicsMeshes(Record->CollisionSnapshot().RequestGeneration);
					Record->bDelivered = true;
					Record->Diagnostic.Status = *Record->Terminal;
					Record->Diagnostic.Phase = EStaticMeshCompilationPhase::Terminal;
					if (!Record->Work->IsCollision()) History.push_back(Record->Diagnostic);
					if (History.size() > MaximumHistory) History.pop_front();
					auto Completion = std::move(Record->Completion);
					auto PhysicsCompletion = std::move(Record->PhysicsCompletion);
					Record->PreparePublication = {};
					if (Record->bRequeue && Mesh && bAccepting)
						Requeues.emplace_back(Record->Diagnostic.Owner, FStaticMeshCompilationRequest{
							.Source = Mesh->GetSource().GetIdentity() == Record->RenderSnapshot().SourceIdentity
								? Record->RequestedSource : Mesh->GetSource(),
							.Priority = Record->Priority, .bMarkPackageDirty = Record->bMarkPackageDirty});
					Record->RequestedSource = {};
					Record->Snapshot = {};
					Record->PreparedMaterialSlots.reset();
					Record->ImportState.reset();
					++Result.ProcessedCompletionCount;
					if (PhysicsCompletion) PhysicsCompletion(*Record->Terminal == EStaticMeshCompilationStatus::Succeeded);
					if (Completion)
					{
						FStaticMeshCompilationResult Completed{.RequestId = Record->Diagnostic.RequestId, .Status = *Record->Terminal};
						if (Record->Diagnostic.Error && (Completed.Status == EStaticMeshCompilationStatus::Failed
							|| Completed.Status == EStaticMeshCompilationStatus::Cancelled))
							Completed.Errors.push_back(Record->Diagnostic.Error->ToString());
						Completion(Completed);
					}
				}
				std::erase_if(Records, [&](const auto& Record) {
					if (!Record->bDelivered || !Record->Work->Done.load(std::memory_order_acquire)
						|| (Record->Task.IsValid() && !Record->Task.IsComplete())) return false;
					Record->Work->ReleaseOutput();
					ReservedBytes -= Record->Diagnostic.ReservedBytes;
					return true;
				});
				for (auto& [Owner, Request] : Requeues)
					if (auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Owner)); Mesh && !HasPending(*Mesh))
					{
						static_cast<void>(Submit(*Mesh, std::move(Request), {}));
					}
				Admit();
				return Result;
			}
			auto Finish(std::span<const FObjectKey> Owners, EStaticMeshCompilationProducts Products = EStaticMeshCompilationProducts::All) -> FAssetCompileProcessResult
			{
				FAssetCompileProcessResult Result;
				while (std::ranges::any_of(Records, [&](const auto& Record) { return Selected(*Record, Owners, Products); }))
				{
					auto Batch = Pump(Owners, std::numeric_limits<uint32>::max(), FClock::time_point::max(), Products);
					Result.ProcessedCompletionCount += Batch.ProcessedCompletionCount;
					Result.SuccessfullyCompiledAssets.insert(Result.SuccessfullyCompiledAssets.end(),
						Batch.SuccessfullyCompiledAssets.begin(), Batch.SuccessfullyCompiledAssets.end());
					if (Batch.ProcessedCompletionCount == 0)
					{
						std::unique_lock Lock(Workers->Mutex);
						Workers->Changed.wait_for(Lock, std::chrono::milliseconds(1));
					}
				}
				return Result;
			}

			std::vector<std::shared_ptr<FRecord>> Records;
			std::deque<FStaticMeshCompilationDiagnostic> History;
			std::shared_ptr<FWorkerState> Workers = std::make_shared<FWorkerState>();
			FTaskScope Scope;
			FObjectKey PublishingOwner;
			uint64 NextRequest = 1;
			uint64 LastPumpIdentity = 0;
			uint32 PumpCount = 0;
			FClock::time_point PumpDeadline;
			uint64 ReservedBytes = 0;
			uint32 InteractiveBurst = 0;
			bool bAccepting = false;
			bool bShutdown = true;
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
	}
	auto FormatStaticMeshCompilationDiagnostic(const FStaticMeshCompilationDiagnostic& Diagnostic) -> std::string
	{
		auto Message = Diagnostic.Error ? Diagnostic.Error->ToString() : std::string{};
		for (const auto& Error : Diagnostic.CacheErrors)
		{
			if (!Message.empty()) Message += "\n";
			Message += Error.ToString();
		}
		const auto Limit = MaximumStaticMeshBuildDiagnosticBytes
			- std::min(MaximumStaticMeshBuildDiagnosticBytes, Diagnostic.Descriptor.ProducerIdentity.size());
		Message.resize(std::min(Message.size(), Limit));
		return Message;
	}

	auto FStaticMeshCompilationResult::ToString() const -> std::string
	{
		if (!Errors.empty()) return FormatStaticMeshBuildMessages(Errors);
		switch (Status)
		{
		case EStaticMeshCompilationStatus::Succeeded: return {};
		case EStaticMeshCompilationStatus::Failed: return "StaticMesh compilation failed.";
		case EStaticMeshCompilationStatus::Cancelled: return "StaticMesh compilation was cancelled.";
		case EStaticMeshCompilationStatus::Superseded: return "StaticMesh compilation was superseded.";
		}
		return {};
	}

	auto DStaticMesh::AsyncBuild(FStaticMeshCompilationRequest Request,
		FStaticMeshCompilationCompletion Completion) -> std::expected<void, std::vector<std::string>>
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) return Manager->Submit(*this, std::move(Request), std::move(Completion));
		return std::unexpected(std::vector<std::string>{"The StaticMesh compiling manager is unavailable."});
	}
	auto CanJoinStaticMeshCompilation(const DStaticMesh& Mesh, const FStaticMeshSource& Source) -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->CanJoin(Mesh, Source);
	}
	auto HasPendingStaticMeshSourceMutation(const DStaticMesh& Mesh) -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->HasSourceMutation(Mesh);
	}
	auto HasPendingStaticMeshCompilation(const DStaticMesh& Mesh) -> bool
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager && Manager->HasPending(Mesh);
	}
	auto GetStaticMeshCompilationDiagnostic(const DStaticMesh& Mesh) -> FStaticMeshCompilationDiagnostic
	{
		CheckOwnerThread();
		const auto Manager = GManager.lock();
		return Manager ? Manager->Diagnostic(Mesh) : FStaticMeshCompilationDiagnostic{};
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
	auto FinishStaticMeshCompilation(DStaticMesh& Mesh, EStaticMeshCompilationProducts Products) -> FAssetCompileProcessResult
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) return Manager->FinishForMesh(Mesh, Products);
		return {};
	}
	auto SubmitPhysicsMeshCompilation(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask, bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion)
		-> std::expected<void, FPhysicsCookFailure>
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) return Manager->SubmitCollision(Body, std::move(InputTask), bPersistDerivedData, std::move(Completion));
		return std::unexpected(FPhysicsCookFailure{"Physics mesh compiling service is unavailable.", EPhysicsCookStage::Scheduling});
	}
	auto CancelPhysicsMeshCompilation(DBodySetup& Body) -> void
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) Manager->CancelCollision(Body);
	}
	auto FinishPhysicsMeshCompilation(DBodySetup& Body) -> void
	{
		CheckOwnerThread();
		DObject* Object = &Body;
		if (auto Manager = GManager.lock()) Manager->FinishCompilationForObjects({&Object, 1});
	}

	auto CancelStaticMeshCompilation(DStaticMesh& Mesh) -> void
	{
		CheckOwnerThread();
		DObject* Object = &Mesh;
		if (auto Manager = GManager.lock()) Manager->MarkCompilationAsCanceled({&Object, 1});
	}
	auto NotifyStaticMeshCompilationMutation(DStaticMesh& Mesh) -> void
	{
		CheckOwnerThread();
		if (auto Manager = GManager.lock()) Manager->Mutated(Mesh);
	}
}

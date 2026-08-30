#include "Asset/CookedMeshLoadManager.h"

#include "DObject/Object.h"
#include "Threading/RunnableThread.h"
namespace Durin::Asset
{
	namespace
	{
		auto SameOwner(FObjectHandle Left, FObjectHandle Right) -> bool
		{
			return Left.Index == Right.Index && Left.Generation == Right.Generation;
		}

		enum class EFlightPhase : uint8 { Reading, Working, CompletionQueued };

		struct FFlight
		{
			uint64 Id = 0;
			FCookedMeshLoadIdentity Identity;
			uint64 EstimatedBytes = 0;
			std::vector<FPackageResourceRequest> Reads;
			FCookedMeshWorker Worker;
			FCookedMeshCurrentPredicate IsCurrent;
			FCookedMeshPublisher Publish;
			FCookedMeshTerminalCallback OnTerminal;
			FTaskCancellationSource Cancellation;
			FTaskHandle Task;
			std::atomic<EFlightPhase> Phase{EFlightPhase::Reading};
		};

		struct FCompletion
		{
			std::shared_ptr<FFlight> Flight;
			FCookedMeshWorkerResult Result;
			ECookedMeshTerminalState State = ECookedMeshTerminalState::Succeeded;
		};

		struct FPendingRequest
		{
			FCookedMeshLoadRequest Request;
			uint64 EstimatedBytes = 0;
		};

		std::mutex GManagerMutex;
		std::unique_ptr<FCookedMeshLoadManager> GManager;
	}

	struct FCookedMeshLoadManager::FState
	{
		explicit FState(FCookedMeshLoadManagerConfig InConfig)
			: Config(InConfig) {}

		auto QueueCompletion(FCompletion Completion) -> void
		{
			std::scoped_lock Lock(Mutex);
			if (Completion.Flight->Phase.exchange(
				EFlightPhase::CompletionQueued, std::memory_order_acq_rel)
				== EFlightPhase::CompletionQueued) return;
			if (Completion.State == ECookedMeshTerminalState::Succeeded)
			{
				const uint64 Bytes = Completion.Result.RetainedBytes;
				if (Bytes == 0 || Bytes > Config.MaxPendingCompletionBytes
					|| PendingCompletionBytes > Config.MaxPendingCompletionBytes - Bytes)
				{
					Completion.Result.Product.reset();
					Completion.Result.RetainedBytes = 0;
					Completion.Result.Message =
						"Cooked mesh result exceeds the bounded completion mailbox.";
					Completion.State = ECookedMeshTerminalState::Failed;
				}
				else PendingCompletionBytes += Bytes;
			}
			Mailbox.push_back(std::move(Completion));
		}

		auto Find(const FCookedMeshLoadIdentity& Identity) const
			-> std::shared_ptr<FFlight>
		{
			const auto It = std::ranges::find_if(Flights,
				[&](const std::shared_ptr<FFlight>& Flight) {
					return Flight->Identity == Identity;
				});
			return It == Flights.end() ? nullptr : *It;
		}

		auto FindPending(const FCookedMeshLoadIdentity& Identity)
			-> std::vector<FPendingRequest>::iterator
		{
			return std::ranges::find_if(PendingRequests,
				[&](const FPendingRequest& Pending) {
					return Pending.Request.Identity == Identity;
				});
		}

		auto HasOwner(FObjectHandle Owner) const -> bool
		{
			return std::ranges::any_of(Flights,
				[Owner](const std::shared_ptr<FFlight>& Flight) {
					return SameOwner(Flight->Identity.Owner, Owner);
				}) || std::ranges::any_of(PendingRequests,
				[Owner](const FPendingRequest& Pending) {
					return SameOwner(Pending.Request.Identity.Owner, Owner);
				});
		}

		auto StartFlight(FCookedMeshLoadRequest Request, uint64 EstimatedBytes) -> void
		{
			auto Flight = std::make_shared<FFlight>();
			Flight->Id = NextId++;
			Flight->Identity = Request.Identity;
			Flight->EstimatedBytes = EstimatedBytes;
			Flight->Worker = std::move(Request.Worker);
			Flight->IsCurrent = std::move(Request.IsCurrent);
			Flight->Publish = std::move(Request.Publish);
			Flight->OnTerminal = std::move(Request.OnTerminal);
			Flight->Reads.reserve(Request.Fields.size());
			for (FBulkData& Field : Request.Fields)
				Flight->Reads.push_back(Field.ReloadAsync());
			InFlightEstimatedBytes += EstimatedBytes;
			Flights.push_back(std::move(Flight));
		}

		auto CanStart(uint64 EstimatedBytes) const -> bool
		{
			if (Flights.size() >= Config.MaxConcurrentRequests) return false;
			if (Flights.empty() && EstimatedBytes > Config.MaxEstimatedBytes) return true;
			return EstimatedBytes <= Config.MaxEstimatedBytes
				&& InFlightEstimatedBytes <= Config.MaxEstimatedBytes - EstimatedBytes;
		}

		auto PromotePending() -> void
		{
			for (auto It = PendingRequests.begin(); It != PendingRequests.end();)
			{
				if (!CanStart(It->EstimatedBytes))
				{
					++It;
					continue;
				}
				FCookedMeshLoadRequest Request = std::move(It->Request);
				const uint64 EstimatedBytes = It->EstimatedBytes;
				check(PendingRequestEstimatedBytes >= EstimatedBytes);
				PendingRequestEstimatedBytes -= EstimatedBytes;
				It = PendingRequests.erase(It);
				StartFlight(std::move(Request), EstimatedBytes);
				if (Flights.size() >= Config.MaxConcurrentRequests) break;
			}
		}

		auto RemoveFlight(const std::shared_ptr<FFlight>& Flight) -> void
		{
			const auto It = std::ranges::find(Flights, Flight);
			if (It == Flights.end()) return;
			check(InFlightEstimatedBytes >= Flight->EstimatedBytes);
			InFlightEstimatedBytes -= Flight->EstimatedBytes;
			Flights.erase(It);
		}

		mutable std::mutex Mutex;
		FCookedMeshLoadManagerConfig Config;
		ECookedMeshManagerState ManagerState = ECookedMeshManagerState::Stopped;
		FTaskScope Scope;
		FTaskAttribution Attribution;
		uint64 NextId = 1;
		uint64 InFlightEstimatedBytes = 0;
		uint64 PendingRequestEstimatedBytes = 0;
		uint64 PendingCompletionBytes = 0;
		std::vector<std::shared_ptr<FFlight>> Flights;
		std::vector<FPendingRequest> PendingRequests;
		std::deque<FCompletion> Mailbox;
		FCookedMeshLoadDiagnostics Diagnostics;
	};

	FCookedMeshLoadManager::FCookedMeshLoadManager(FCookedMeshLoadManagerConfig Config)
		: State(std::make_shared<FState>(Config))
	{
	}

	FCookedMeshLoadManager::~FCookedMeshLoadManager()
	{
		Shutdown();
	}

	auto FCookedMeshLoadManager::Initialize() -> bool
	{
		CheckGameThread();
		std::scoped_lock Lock(State->Mutex);
		if (State->ManagerState == ECookedMeshManagerState::Accepting) return true;
		if (!IsTaskSchedulerRunning() || State->Config.MaxConcurrentRequests == 0
			|| State->Config.MaxEstimatedBytes == 0
			|| State->Config.MaxPendingRequests == 0
			|| State->Config.MaxPendingEstimatedBytes == 0
			|| State->Config.MaxPendingCompletionBytes == 0
			|| State->Config.MaxIoPollsPerPump == 0
			|| State->Config.MaxCompletionsPerPump == 0) return false;
		State->Scope = CreateTaskScope();
		if (!State->Scope.IsValid()) return false;
		State->Attribution = RegisterTaskAttribution("Engine", "CookedMeshLoad");
		State->ManagerState = ECookedMeshManagerState::Accepting;
		return true;
	}

	auto FCookedMeshLoadManager::Submit(FCookedMeshLoadRequest Request) -> bool
	{
		CheckGameThread();
		if (IsObjectHandleNull(Request.Identity.Owner)
			|| Request.Identity.LoadGeneration == 0
			|| Request.Identity.ResourceRevision == 0
			|| Request.Fields.empty() || !Request.Worker || !Request.Publish)
		{
			std::scoped_lock Lock(State->Mutex);
			++State->Diagnostics.RejectedCount;
			return false;
		}
		uint64 EstimatedBytes = 0;
		for (const FBulkData& Field : Request.Fields)
		{
			const uint64 Bytes = Field.GetMetadata().LogicalSize;
			if (Bytes == 0 || EstimatedBytes > MaximumBulkDataBytes - Bytes)
			{
				std::scoped_lock Lock(State->Mutex);
				++State->Diagnostics.RejectedCount;
				return false;
			}
			EstimatedBytes += Bytes;
		}

		{
			std::scoped_lock Lock(State->Mutex);
			if (State->ManagerState != ECookedMeshManagerState::Accepting)
			{
				++State->Diagnostics.RejectedCount;
				return false;
			}

			const auto SameAsset = [&](const FCookedMeshLoadIdentity& Existing) {
				return SameOwner(Existing.Owner, Request.Identity.Owner)
					&& Existing.Family == Request.Identity.Family;
			};
			uint64 CurrentGeneration = 0;
			for (const std::shared_ptr<FFlight>& Existing : State->Flights)
				if (SameAsset(Existing->Identity))
					CurrentGeneration = std::max(
						CurrentGeneration, Existing->Identity.LoadGeneration);
			for (const FPendingRequest& Existing : State->PendingRequests)
				if (SameAsset(Existing.Request.Identity))
					CurrentGeneration = std::max(CurrentGeneration,
						Existing.Request.Identity.LoadGeneration);
			if (CurrentGeneration != 0
				&& Request.Identity.LoadGeneration < CurrentGeneration)
			{
				++State->Diagnostics.RejectedCount;
				return false;
			}
			if (CurrentGeneration == Request.Identity.LoadGeneration)
			{
				if (State->Find(Request.Identity)
					|| State->FindPending(Request.Identity) != State->PendingRequests.end())
				{
					++State->Diagnostics.CoalescedCount;
					return true;
				}
				++State->Diagnostics.RejectedCount;
				return false;
			}
			const bool bSupersedesFlight = std::ranges::any_of(State->Flights,
				[&](const std::shared_ptr<FFlight>& Existing) {
					return SameAsset(Existing->Identity);
				});
			const auto PendingIt = std::ranges::find_if(State->PendingRequests,
				[&](const FPendingRequest& Existing) {
					return SameAsset(Existing.Request.Identity);
				});
			if (bSupersedesFlight || PendingIt != State->PendingRequests.end())
			{
				const uint64 ReplacedPendingBytes =
					PendingIt == State->PendingRequests.end()
						? 0 : PendingIt->EstimatedBytes;
				const uint64 ProspectivePendingBytes =
					State->PendingRequestEstimatedBytes - ReplacedPendingBytes;
				if ((PendingIt == State->PendingRequests.end()
						&& State->PendingRequests.size() >= State->Config.MaxPendingRequests)
					|| EstimatedBytes > State->Config.MaxPendingEstimatedBytes
					|| ProspectivePendingBytes
						> State->Config.MaxPendingEstimatedBytes - EstimatedBytes)
				{
					++State->Diagnostics.RejectedCount;
					return false;
				}
				for (const std::shared_ptr<FFlight>& Existing : State->Flights)
					if (SameAsset(Existing->Identity))
					{
						Existing->Cancellation.RequestCancellation();
						for (FPackageResourceRequest& Read : Existing->Reads) Read.Cancel();
						if (Existing->Task.IsValid()) CancelTask(Existing->Task);
					}
				if (PendingIt != State->PendingRequests.end())
				{
					State->PendingRequestEstimatedBytes -= PendingIt->EstimatedBytes;
					State->PendingRequests.erase(PendingIt);
					++State->Diagnostics.CancelledCount;
				}
				State->PendingRequestEstimatedBytes += EstimatedBytes;
				State->PendingRequests.push_back(
					{std::move(Request), EstimatedBytes});
				++State->Diagnostics.SupersededCount;
				++State->Diagnostics.AcceptedCount;
				return true;
			}

			if (!State->CanStart(EstimatedBytes))
			{
				++State->Diagnostics.RejectedCount;
				return false;
			}
			State->StartFlight(std::move(Request), EstimatedBytes);
			++State->Diagnostics.AcceptedCount;
		}
		return true;
	}

	auto FCookedMeshLoadManager::Pump() -> uint32
	{
		CheckGameThread();
		std::vector<std::shared_ptr<FFlight>> Snapshot;
		{
			std::scoped_lock Lock(State->Mutex);
			Snapshot = State->Flights;
		}
		uint32 Polled = 0;
		for (const std::shared_ptr<FFlight>& Flight : Snapshot)
		{
			if (Polled >= State->Config.MaxIoPollsPerPump) break;
			const EFlightPhase Phase = Flight->Phase.load(std::memory_order_acquire);
			if (Phase == EFlightPhase::Reading)
			{
				++Polled;
				if (!std::ranges::all_of(Flight->Reads,
					[](const FPackageResourceRequest& Read) { return Read.IsReady(); })) continue;
				std::vector<FSharedByteBuffer> Buffers;
				Buffers.reserve(Flight->Reads.size());
				std::string ReadError;
				ECookedMeshTerminalState Terminal = ECookedMeshTerminalState::Succeeded;
				for (const FPackageResourceRequest& Read : Flight->Reads)
				{
					FPackageResourceReadResult Result = Read.Wait();
					if (!Result)
					{
						Terminal = Result.Status == EPackageResourceReadStatus::Cancelled
							? ECookedMeshTerminalState::Cancelled
							: ECookedMeshTerminalState::Failed;
						ReadError = std::move(Result.Message);
						break;
					}
					Buffers.push_back(std::move(Result.Buffer));
				}
				Flight->Reads.clear();
				if (Terminal != ECookedMeshTerminalState::Succeeded)
				{
					State->QueueCompletion({Flight,
						{.Message = std::move(ReadError)}, Terminal});
					continue;
				}
				{
					std::scoped_lock Lock(State->Mutex);
					if (State->ManagerState != ECookedMeshManagerState::Accepting)
					{
						Terminal = ECookedMeshTerminalState::Cancelled;
					}
				}
				if (Terminal == ECookedMeshTerminalState::Cancelled)
				{
					State->QueueCompletion({Flight,
						{.Message = "Cooked mesh manager shutdown cancelled decode admission."},
						Terminal});
					continue;
				}
				Flight->Phase.store(EFlightPhase::Working, std::memory_order_release);
				auto SharedState = State;
				Flight->Task = LaunchCancelableTask(
					"CookedMeshDecode",
					[SharedState, Flight, Buffers = std::move(Buffers)](
						const FTaskCancellationToken& Token) mutable {
						if (Token.IsCancellationRequested())
						{
							SharedState->QueueCompletion({Flight, {},
								ECookedMeshTerminalState::Cancelled});
							return;
						}
						FCookedMeshWorkerResult Result = Flight->Worker(Buffers, Token);
						const bool bCancelled = Token.IsCancellationRequested();
						const ECookedMeshTerminalState Terminal = bCancelled
							? ECookedMeshTerminalState::Cancelled
							: Result ? ECookedMeshTerminalState::Succeeded
							: ECookedMeshTerminalState::Failed;
						SharedState->QueueCompletion(
							{Flight, std::move(Result), Terminal});
					}, {.CancellationToken = Flight->Cancellation.GetToken(),
						.Attribution = State->Attribution,
						.Scope = State->Scope.GetToken()});
				if (!Flight->Task.IsValid())
					State->QueueCompletion({Flight,
						{.Message = "Cooked mesh worker admission was rejected."},
						ECookedMeshTerminalState::Rejected});
			}
			else if (Phase == EFlightPhase::Working && Flight->Task.IsComplete())
			{
				State->QueueCompletion({Flight,
					{.Message = Flight->Task.GetDiagnostics().Diagnostic},
					Flight->Task.GetState() == ETaskState::Failed
						? ECookedMeshTerminalState::Failed
						: ECookedMeshTerminalState::Cancelled});
			}
		}

		uint32 Processed = 0;
		while (Processed < State->Config.MaxCompletionsPerPump)
		{
			FCompletion Completion;
			{
				std::scoped_lock Lock(State->Mutex);
				if (State->Mailbox.empty()) break;
				Completion = std::move(State->Mailbox.front());
				State->Mailbox.pop_front();
				check(State->PendingCompletionBytes >= Completion.Result.RetainedBytes);
				State->PendingCompletionBytes -= Completion.Result.RetainedBytes;
			}
			ECookedMeshTerminalState Terminal = Completion.State;
			std::string TerminalMessage = std::move(Completion.Result.Message);
			bool bAccepting = false;
			if (Terminal == ECookedMeshTerminalState::Succeeded)
			{
				{
					std::scoped_lock Lock(State->Mutex);
					bAccepting = State->ManagerState == ECookedMeshManagerState::Accepting;
					if (!bAccepting)
						Terminal = ECookedMeshTerminalState::Cancelled;
				}
			}
			else
			{
				std::scoped_lock Lock(State->Mutex);
				bAccepting = State->ManagerState == ECookedMeshManagerState::Accepting;
			}
			DObject* Owner = nullptr;
			bool bCurrent = false;
			if (Terminal == ECookedMeshTerminalState::Succeeded)
			{
				Owner = ResolveObjectHandle(Completion.Flight->Identity.Owner);
				bCurrent = Owner && (!Completion.Flight->IsCurrent
					|| Completion.Flight->IsCurrent(*Owner, Completion.Flight->Identity));
				if (!bCurrent)
				{
					Terminal = ECookedMeshTerminalState::Stale;
				}
				else
				{
					std::string Error;
					if (!Completion.Flight->Publish(*Owner,
						Completion.Flight->Identity, std::move(Completion.Result.Product), Error))
					{
						Terminal = ECookedMeshTerminalState::Failed;
						TerminalMessage = std::move(Error);
					}
				}
			}
			if (Terminal != ECookedMeshTerminalState::Succeeded
				&& Terminal != ECookedMeshTerminalState::Stale
				&& Completion.Flight->OnTerminal)
			{
				if (!Owner) Owner = ResolveObjectHandle(Completion.Flight->Identity.Owner);
				if (Owner && (!Completion.Flight->IsCurrent
					|| Completion.Flight->IsCurrent(*Owner, Completion.Flight->Identity)))
				{
					Completion.Flight->OnTerminal(*Owner, Completion.Flight->Identity,
						Terminal, TerminalMessage);
				}
			}
			{
				std::scoped_lock Lock(State->Mutex);
				State->RemoveFlight(Completion.Flight);
				switch (Terminal)
				{
				case ECookedMeshTerminalState::Succeeded: ++State->Diagnostics.SucceededCount; break;
				case ECookedMeshTerminalState::Failed:
				case ECookedMeshTerminalState::Rejected: ++State->Diagnostics.FailedCount; break;
				case ECookedMeshTerminalState::Cancelled: ++State->Diagnostics.CancelledCount; break;
				case ECookedMeshTerminalState::Stale: ++State->Diagnostics.StaleCount; break;
				}
				State->PromotePending();
			}
			++Processed;
		}
		return Processed;
	}

	auto FCookedMeshLoadManager::Cancel(FObjectHandle Owner) -> bool
	{
		CheckGameThread();
		std::scoped_lock Lock(State->Mutex);
		bool bFound = false;
		for (const std::shared_ptr<FFlight>& Flight : State->Flights)
		{
			if (!SameOwner(Flight->Identity.Owner, Owner)) continue;
			bFound = true;
			Flight->Cancellation.RequestCancellation();
			for (FPackageResourceRequest& Read : Flight->Reads) Read.Cancel();
			if (Flight->Task.IsValid()) CancelTask(Flight->Task);
		}
		for (auto It = State->PendingRequests.begin(); It != State->PendingRequests.end();)
		{
			if (!SameOwner(It->Request.Identity.Owner, Owner))
			{
				++It;
				continue;
			}
			bFound = true;
			State->PendingRequestEstimatedBytes -= It->EstimatedBytes;
			It = State->PendingRequests.erase(It);
			++State->Diagnostics.CancelledCount;
		}
		return bFound;
	}

	auto FCookedMeshLoadManager::Finish(FObjectHandle Owner) -> void
	{
		CheckGameThread();
		for (;;)
		{
			{
				std::scoped_lock Lock(State->Mutex);
				if (!State->HasOwner(Owner)) return;
			}
			Pump();
			std::this_thread::yield();
		}
	}

	auto FCookedMeshLoadManager::StopAdmission() -> void
	{
		CheckGameThread();
		std::scoped_lock Lock(State->Mutex);
		if (State->ManagerState == ECookedMeshManagerState::Accepting)
			State->ManagerState = ECookedMeshManagerState::Draining;
	}

	auto FCookedMeshLoadManager::Shutdown() -> void
	{
		if (!State) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		{
			std::scoped_lock Lock(State->Mutex);
			if (State->ManagerState == ECookedMeshManagerState::Stopped) return;
			State->ManagerState = ECookedMeshManagerState::Draining;
			for (const std::shared_ptr<FFlight>& Flight : State->Flights)
			{
				Flight->Cancellation.RequestCancellation();
				for (FPackageResourceRequest& Read : Flight->Reads) Read.Cancel();
				if (Flight->Task.IsValid()) CancelTask(Flight->Task);
			}
			State->Diagnostics.CancelledCount += State->PendingRequests.size();
			State->PendingRequests.clear();
			State->PendingRequestEstimatedBytes = 0;
		}
		State->Scope.Close(ETaskScopeCloseMode::Cancel);
		while (true)
		{
			{
				std::scoped_lock Lock(State->Mutex);
				if (State->Flights.empty()) break;
			}
			Pump();
			std::this_thread::yield();
		}
		(void)State->Scope.Wait();
		std::scoped_lock Lock(State->Mutex);
		State->Mailbox.clear();
		State->PendingCompletionBytes = 0;
		State->Scope = {};
		State->ManagerState = ECookedMeshManagerState::Stopped;
	}

	auto FCookedMeshLoadManager::GetDiagnostics() const -> FCookedMeshLoadDiagnostics
	{
		std::scoped_lock Lock(State->Mutex);
		FCookedMeshLoadDiagnostics Result = State->Diagnostics;
		Result.State = State->ManagerState;
		Result.InFlightCount = static_cast<uint32>(State->Flights.size());
		Result.PendingRequestCount = static_cast<uint32>(State->PendingRequests.size());
		Result.PendingCompletionCount = static_cast<uint32>(State->Mailbox.size());
		Result.InFlightEstimatedBytes = State->InFlightEstimatedBytes;
		Result.PendingRequestEstimatedBytes = State->PendingRequestEstimatedBytes;
		Result.PendingCompletionBytes = State->PendingCompletionBytes;
		return Result;
	}

	auto InitializeCookedMeshLoadManager() -> bool
	{
		CheckGameThread();
		std::scoped_lock Lock(GManagerMutex);
		if (!GManager) GManager = std::make_unique<FCookedMeshLoadManager>();
		return GManager->Initialize();
	}

	auto PumpCookedMeshLoadManager() -> uint32
	{
		CheckGameThread();
		std::scoped_lock Lock(GManagerMutex);
		return GManager ? GManager->Pump() : 0;
	}

	auto ShutdownCookedMeshLoadManager() -> void
	{
		CheckGameThread();
		std::unique_ptr<FCookedMeshLoadManager> Manager;
		{
			std::scoped_lock Lock(GManagerMutex);
			Manager = std::move(GManager);
		}
		if (Manager) Manager->Shutdown();
	}

	auto GetCookedMeshLoadManager() -> FCookedMeshLoadManager*
	{
		std::scoped_lock Lock(GManagerMutex);
		return GManager.get();
	}
}

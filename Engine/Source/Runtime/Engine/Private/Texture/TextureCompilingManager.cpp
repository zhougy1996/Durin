#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/TextureCompilingManager.h"
#include "TextureBuildInternal.h"
#include "Asset/AssetCompilationOperation.h"

#include "Texture/Texture2DBuild.h"
#include "Texture/ITextureCompressorModule.h"

#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		using FClock = std::chrono::steady_clock;
		auto NowNanoseconds() -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				FClock::now().time_since_epoch()).count());
		}

		auto SaturatingAdd(uint64 Left, uint64 Right) -> uint64
		{
			return Right > std::numeric_limits<uint64>::max() - Left
				? std::numeric_limits<uint64>::max() : Left + Right;
		}

		auto SaturatingMultiply(uint64 Left, uint64 Right) -> uint64
		{
			return Left != 0 && Right > std::numeric_limits<uint64>::max() / Left
				? std::numeric_limits<uint64>::max() : Left * Right;
		}

		auto EstimateBuildBytes(const FTexture2DCompilationWork& Request) -> uint64
		{
			if (Request.PlatformCache) return Request.PlatformCache->EstimatedBytes;
			uint64 PixelCount = SaturatingMultiply(Request.Build.Source.GetWidth(), Request.Build.Source.GetHeight());
			uint64 WorkingBytes = SaturatingMultiply(PixelCount, 12);
			if (WorkingBytes == 0)
				WorkingBytes = std::max<uint64>(
					SaturatingMultiply(Request.Build.Source.GetDecodedPayloadSize(), 3),
					64ull * 1024ull * 1024ull);
			return SaturatingAdd(Request.Build.Source.GetDecodedPayloadSize(), WorkingBytes);
		}

		auto PlatformDataBytes(const FTexturePlatformData& PlatformData) -> uint64
		{
			uint64 Bytes = 0;
			for (const FTexture2DMipData& Mip : PlatformData.Mips)
				Bytes = SaturatingAdd(Bytes, Mip.Pixels.size());
			return Bytes;
		}

	}

	struct FTextureCompilingManager::FQueueState final
		: public std::enable_shared_from_this<FTextureCompilingManager::FQueueState>
	{
		// The manager owns delivery; the task represents only background computation.
		struct FRequestState
		{
			FTexture2DCompilationWork Request;
			FTexture2DCompilationWorkCompletion Completion;
			std::shared_ptr<FTexture2DCompilationProgress> Progress;
			Tasks::TTask<void> Task;
			std::shared_ptr<FAssetCompilationOperation> BuildOperation;
			std::optional<FTexture2DCompilationWorkResult> Mailbox;
			bool bDelivered = false;
			bool bComputeBudgetCharged = false;
			std::atomic<bool> bCancellationRequested = false;
			uint64 EstimatedBytes = 0;
		};

		explicit FQueueState(FTextureCompilingManagerConfig InConfig)
			: Config(InConfig)
		{
			Config.MaxWorkers = std::max(Config.MaxWorkers, 1u);
			Config.MaxPendingRequests = std::max(Config.MaxPendingRequests, 1u);
			Config.InteractiveBurstLimit = std::max(Config.InteractiveBurstLimit, 1u);
			Config.InFlightByteBudget = std::max<uint64>(Config.InFlightByteBudget, 1);
		}

		auto Submit(FTexture2DCompilationWork Request, FTexture2DCompilationWorkCompletion Completion) -> uint64
		{
			if (!Completion || IsObjectKeyNull(Request.Owner)
				|| Request.AssetIdentity.empty() || (!Request.Build.Source.IsValid() && !Request.PlatformCache)
				|| Request.Build.Source.GetIdentity().IsZero()) return 0;
			auto RequestState = std::make_shared<FRequestState>();
			RequestState->Request = std::move(Request);
			RequestState->Progress = std::move(RequestState->Request.Progress);
			require(RequestState->Progress != nullptr);
			if (!ITextureCompressorModule::Get()) return 0;
			RequestState->Completion = std::move(Completion);
			RequestState->EstimatedBytes = EstimateBuildBytes(RequestState->Request);
			RequestState->Progress->EnqueueNanoseconds = NowNanoseconds();
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingRequests || PendingRequestCount >= Config.MaxPendingRequests) return 0;
				RequestState->Progress->Diagnostic.RequestId = NextRequestId++;
				++PendingRequestCount;
				RequestState->Progress->Diagnostic.AssetIdentity = RequestState->Request.AssetIdentity;
				RequestState->Progress->Diagnostic.Phase = ETexture2DCompilationPhase::Queued;
				RequestState->Progress->Diagnostic.Metrics.EstimatedBytes = RequestState->EstimatedBytes;
				Requests.emplace(RequestState->Progress->Diagnostic.RequestId, RequestState);
				if (RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive)
					InteractiveQueue.push_back(RequestState);
				else BackgroundQueue.push_back(RequestState);
			}
			Admit();
			return RequestState->Progress->Diagnostic.RequestId;
		}

		auto SelectNextRequestLocked() -> std::shared_ptr<FRequestState>
		{
			auto PopAvailable = [](std::deque<std::shared_ptr<FRequestState>>& Queue) {
				while (!Queue.empty())
				{
					std::shared_ptr<FRequestState> RequestState = std::move(Queue.front());
					Queue.pop_front();
					if (RequestState) return RequestState;
				}
				return std::shared_ptr<FRequestState>{};
			};
			const bool bChooseBackground = !BackgroundQueue.empty()
				&& (InteractiveQueue.empty() || ConsecutiveInteractive >= Config.InteractiveBurstLimit);
			if (bChooseBackground)
			{
				return PopAvailable(BackgroundQueue);
			}
			if (!InteractiveQueue.empty())
			{
				return PopAvailable(InteractiveQueue);
			}
			return PopAvailable(BackgroundQueue);
		}

		static auto IsReady(const FRequestState& RequestState) -> bool
		{
			if (RequestState.BuildOperation)
			{
				// A posted product is not enough: nested completion includes callback/capture retirement.
				return RequestState.Task.IsValid() && RequestState.Task.GetCompletion().IsReady();
			}
			return RequestState.Mailbox.has_value(); // Canceled before worker admission.
		}

		auto ReleaseComputeBudgetLocked(FRequestState& RequestState) -> void
		{
			if (!RequestState.bComputeBudgetCharged) return;
			RequestState.bComputeBudgetCharged = false;
			require(RunningCount > 0 && InFlightEstimatedBytes >= RequestState.EstimatedBytes);
			--RunningCount;
			InFlightEstimatedBytes -= RequestState.EstimatedBytes;
		}

		auto Admit() -> void
		{
			std::vector<std::shared_ptr<FRequestState>> Admitted;
			std::vector<std::shared_ptr<FRequestState>> Cancelled;
			const bool bSchedulerRunning = IsTaskSchedulerRunning();
			{
				std::lock_guard Lock(Mutex);
				for (const auto& [Id, RequestState] : Requests)
					if (IsReady(*RequestState)) ReleaseComputeBudgetLocked(*RequestState);
				while (!bShutdown && (RunningCount < Config.MaxWorkers || !bSchedulerRunning))
				{
					std::shared_ptr<FRequestState> RequestState = SelectNextRequestLocked();
					if (!RequestState) break;
					if (!bSchedulerRunning || RequestState->bCancellationRequested.load(std::memory_order_acquire))
					{
						Cancelled.push_back(std::move(RequestState));
						continue;
					}
					const bool bMayRunAlone = RunningCount == 0 && InFlightEstimatedBytes == 0;
					if (!bMayRunAlone
						&& RequestState->EstimatedBytes > Config.InFlightByteBudget - std::min(
							InFlightEstimatedBytes, Config.InFlightByteBudget))
					{
						if (RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive)
							InteractiveQueue.push_front(std::move(RequestState));
						else BackgroundQueue.push_front(std::move(RequestState));
						break;
					}
					RequestState->bComputeBudgetCharged = true;
					++RunningCount;
					InFlightEstimatedBytes = SaturatingAdd(InFlightEstimatedBytes, RequestState->EstimatedBytes);
					if (RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive)
						++ConsecutiveInteractive;
					else ConsecutiveInteractive = 0;
					Admitted.push_back(std::move(RequestState));
				}
			}
			for (const std::shared_ptr<FRequestState>& RequestState : Cancelled)
				CompleteWithoutWorker(RequestState, DerivedData::EStatus::Canceled);
			for (const std::shared_ptr<FRequestState>& RequestState : Admitted) Launch(RequestState);
		}

		auto Launch(const std::shared_ptr<FRequestState>& RequestState) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				RequestState->BuildOperation = std::make_shared<FAssetCompilationOperation>(
					RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive
						? DerivedData::ERequestPriority::High : DerivedData::ERequestPriority::Low);
			}
			const std::shared_ptr<FQueueState> Self = shared_from_this();
			Tasks::FTaskGroup Group(Scope.GetToken());
			Tasks::FTaskExecutionOptions Options;
			Options.DebugName = "Texture2D.Build";
			Options.Attribution = Attribution;
			Options.Priority = RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive
				? ETaskPriority::High : ETaskPriority::Low;
			auto Admission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options,
				[Self, RequestState](Tasks::FTaskContext& Context) {
					try
					{
						Self->RunWorker(RequestState, Context.GetCancellationToken());
						Tasks::AddNested(RequestState->BuildOperation->GetCompletionEvent());
						RequestState->BuildOperation->CloseSubmission();
					}
					catch (...)
					{
						Tasks::AddNested(RequestState->BuildOperation->GetCompletionEvent());
						RequestState->BuildOperation->CloseSubmission();
						throw;
					}
				});
			std::lock_guard Lock(Mutex);
			RequestState->Task = std::move(Admission);
		}

		static auto MakeIssueResult(
			const FRequestState& RequestState,
			DerivedData::EStatus Status, std::string Diagnostic = {}) -> FTexture2DCompilationWorkResult
		{
			return {
				.RequestId = RequestState.Progress->Diagnostic.RequestId,
				.Owner = RequestState.Request.Owner,
				.AssetIdentity = RequestState.Request.AssetIdentity,
				.Status = Status,
				.Diagnostic = std::move(Diagnostic),
				.Metrics = {.EstimatedBytes = RequestState.EstimatedBytes},
				.SourceIdentity = RequestState.Request.Build.Source.GetIdentity(),
				.Settings = RequestState.Request.Build.Settings,
				.bSourceDecoderInvoked = RequestState.Request.bSourceDecoderInvoked};
		}

		auto SetPhase(const std::shared_ptr<FRequestState>& RequestState, ETexture2DCompilationPhase Phase) -> void
		{
			{
				std::lock_guard RequestStateLock(RequestState->Progress->Mutex);
				RequestState->Progress->Diagnostic.Phase = Phase;
			}
			NotifyPhaseHook(RequestState->Progress->Diagnostic.RequestId, Phase);
		}

		auto NotifyPhaseHook(uint64 RequestId, ETexture2DCompilationPhase Phase) -> void
		{
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook;
			{
				std::lock_guard Lock(Mutex);
				Hook = PhaseHookForTests;
			}
			if (Hook) Hook(RequestId, Phase);
		}

		auto IsCancelled(
			const std::shared_ptr<FRequestState>& RequestState,
			const FTaskCancellationToken& Token) const -> bool
		{
			return RequestState->bCancellationRequested.load(std::memory_order_acquire)
				|| Token.IsCancellationRequested();
		}

		auto RunWorker(
			const std::shared_ptr<FRequestState>& RequestState,
			const FTaskCancellationToken& Token) -> void
		{
			require(RequestState->BuildOperation != nullptr);
			FTexture2DCompilationWorkResult Result = MakeIssueResult(
				*RequestState, DerivedData::EStatus::Error);
			const uint64 BuildStart = NowNanoseconds();
			{
				std::lock_guard RequestStateLock(RequestState->Progress->Mutex);
				RequestState->Progress->BuildStartNanoseconds = BuildStart;
				RequestState->Progress->Diagnostic.QueuedNanoseconds = BuildStart - RequestState->Progress->EnqueueNanoseconds;
			}
			const auto Cancel = [&] { return IsCancelled(RequestState, Token); };
			if (Cancel())
			{
				Result.Status = DerivedData::EStatus::Canceled;
				Result.Diagnostic.clear();
				PostResult(RequestState, std::move(Result));
				return;
			}

			SetPhase(RequestState, ETexture2DCompilationPhase::Building);
			if (RequestState->Request.PlatformCache)
			{
				const auto Self = shared_from_this();
				RequestState->Request.PlatformCache->Dispatch(*RequestState->BuildOperation,
					[Self, RequestState, Token] { return Self->IsCancelled(RequestState, Token); },
					[Self, RequestState, PendingResult = std::make_shared<FTexture2DCompilationWorkResult>(std::move(Result)), BuildStart]
					(DerivedData::EStatus Status, std::unique_ptr<FTexturePlatformCacheResult> Product, std::string Diagnostic) mutable {
						auto& Result = *PendingResult;
						try
						{
							Result.Status = Status;
							Result.Diagnostic = std::move(Diagnostic);
							Result.PlatformCache = std::move(Product);
							Result.Metrics.BuildElapsedNanoseconds = NowNanoseconds() - BuildStart;
							Self->UpdateDiagnostic(RequestState, Result);
						}
						catch (...) { Result = MakeIssueResult(*RequestState, DerivedData::EStatus::Error, "Texture build task failed."); }
						Self->PostResult(RequestState, std::move(Result));
					});
				return;
			}
			if (Cancel())
			{
				Result.Status = DerivedData::EStatus::Canceled;
				Result.Diagnostic.clear();
				PostResult(RequestState, std::move(Result));
				return;
			}
			FTexture2DBuildRequest BuildRequest = std::move(RequestState->Request.Build);
			const auto Self = shared_from_this();
			TexturePrivate::BuildTexture2DAsync(std::move(BuildRequest), *RequestState->BuildOperation,
				[Self, RequestState, Token] { return Self->IsCancelled(RequestState, Token); },
				[Self, RequestState, PendingResult = std::make_shared<FTexture2DCompilationWorkResult>(std::move(Result)), BuildStart]
				(DerivedData::EStatus Status, std::unique_ptr<FTexturePlatformData> Product, std::string Diagnostic) mutable {
					auto& Result = *PendingResult;
					try
					{
						Result.Status = Status;
						Result.Diagnostic = std::move(Diagnostic);
						if (Product) Result.Metrics.ResultBytes = PlatformDataBytes(*Product);
						Result.PlatformData = std::move(Product);
						Result.Metrics.BuildElapsedNanoseconds = NowNanoseconds() - BuildStart;
						Self->UpdateDiagnostic(RequestState, Result);
					}
					catch (...)
					{
						Result = MakeIssueResult(*RequestState, DerivedData::EStatus::Error,
							"Texture build task failed.");
					}
					Self->PostResult(RequestState, std::move(Result));
				});
			// The callback posts the only work result; the dispatch task carries no product.
		}

		auto PostResult(const std::shared_ptr<FRequestState>& RequestState,
			FTexture2DCompilationWorkResult Result) -> void
		{
			std::lock_guard Lock(Mutex);
			require(!RequestState->Mailbox.has_value());
			RequestState->Mailbox.emplace(std::move(Result));
		}

		auto CompleteWithoutWorker(
			const std::shared_ptr<FRequestState>& RequestState,
			DerivedData::EStatus Status) -> void
		{
			auto Result = MakeIssueResult(*RequestState, Status);
			UpdateDiagnostic(RequestState, Result);
			PostResult(RequestState, std::move(Result));
		}

		auto UpdateDiagnostic(
			const std::shared_ptr<FRequestState>& RequestState,
			const FTexture2DCompilationWorkResult& Result, bool bNotify = true) -> void
		{
			{
				std::lock_guard RequestStateLock(RequestState->Progress->Mutex);
				RequestState->Progress->Diagnostic.Phase = ETexture2DCompilationPhase::AwaitingApply;
				RequestState->Progress->Diagnostic.Diagnostic = Result.Diagnostic;
				RequestState->Progress->Diagnostic.Metrics = Result.Metrics;
				RequestState->Progress->Diagnostic.bSourceDecoderInvoked = Result.bSourceDecoderInvoked;
				RequestState->Progress->Diagnostic.QueuedNanoseconds = RequestState->Progress->BuildStartNanoseconds != 0
					? RequestState->Progress->BuildStartNanoseconds - RequestState->Progress->EnqueueNanoseconds
					: NowNanoseconds() - RequestState->Progress->EnqueueNanoseconds;
			}
			if (bNotify) NotifyPhaseHook(RequestState->Progress->Diagnostic.RequestId, ETexture2DCompilationPhase::AwaitingApply);
		}

		auto Cancel(uint64 RequestId) -> bool
		{
			std::shared_ptr<FAssetCompilationOperation> Owner;
			bool Changed;
			{
				std::lock_guard Lock(Mutex);
				const auto Iterator = Requests.find(RequestId);
				if (Iterator == Requests.end() || Iterator->second->bDelivered) return false;
				Changed = !Iterator->second->bCancellationRequested.exchange(true, std::memory_order_acq_rel);
				Owner = Iterator->second->BuildOperation;
			}
			// Signal without joining a producer on GameThread; the pump consumes its mailbox.
			if (Owner) Owner->RequestCancel();
			return Changed;
		}

		auto Pump(uint32 MaximumCount,
			std::optional<FClock::time_point> Deadline = {}, uint64 OnlyRequest = 0) -> uint32
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			const auto Self = shared_from_this();
			uint32 Count = 0;
			while (Count < MaximumCount)
			{
				if (Deadline && FClock::now() >= *Deadline) break;
				Admit();
				std::shared_ptr<FRequestState> Ready;
				{
					std::lock_guard Lock(Mutex);
					for (const auto& [Id, RequestState] : Requests)
					{
						if ((!OnlyRequest || OnlyRequest == Id)
							&& !RequestState->bDelivered && IsReady(*RequestState)) { Ready = RequestState; break; }
					}
					if (!Ready) break;
					if (!Ready->Mailbox)
					{
						// A skipped/failed dispatch body cannot post. Reap it into the same mailbox after retirement.
						const auto TaskState = Ready->Task.GetState();
						const bool bCanceled = TaskState == ETaskState::Canceled || Ready->bCancellationRequested.load();
						Ready->Mailbox.emplace(MakeIssueResult(*Ready,
							bCanceled ? DerivedData::EStatus::Canceled : DerivedData::EStatus::Error,
							bCanceled ? std::string{} : "Texture build task failed."));
					}
					// Detach before invoking callbacks, which may submit or shut down the manager.
					ReleaseComputeBudgetLocked(*Ready);
					Ready->bDelivered = true;
				}
				auto Result = std::move(*Ready->Mailbox);
				if (Ready->bCancellationRequested.load())
				{
					Result.Status = DerivedData::EStatus::Canceled;
					Result.Diagnostic.clear();
					Result.PlatformData.reset();
					Result.PlatformCache.reset();
				}
				if (Result.Status == DerivedData::EStatus::Canceled) Result.Diagnostic.clear();
				else if (Result.Status == DerivedData::EStatus::Error && Result.Diagnostic.empty())
					Result.Diagnostic = "Texture build failed.";
				UpdateDiagnostic(Ready, Result, false);
				const uint64 CompletionStart = NowNanoseconds();
				auto Completion = std::move(Ready->Completion);
				try { if (Completion) Completion(std::move(Result)); }
				catch (...) { DURIN_ERROR_CATEGORY("Texture", "Texture2D compilation completion callback threw an exception."); }
				{
					std::lock_guard RequestLock(Ready->Progress->Mutex);
					Ready->Progress->Diagnostic.Metrics.CompletionNanoseconds = NowNanoseconds() - CompletionStart;
				}
				{
					std::lock_guard Lock(Mutex);
					Ready->Task = {};
					Ready->BuildOperation.reset();
					Ready->Mailbox.reset();
					Ready->Request.Build = {};
					Ready->Request.PlatformCache.reset();
					require(PendingRequestCount > 0);
					--PendingRequestCount;
					Requests.erase(Ready->Progress->Diagnostic.RequestId);
				}
				++Count;
			}
			return Count;
		}

		auto WaitForRequest(uint64 RequestId, double TimeoutSeconds) -> bool
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			const auto StartTime = FClock::now();
			for (;;)
			{
				// Polling advances queued computes without applying unrelated asset results.
				Admit();
				{
					std::lock_guard Lock(Mutex);
					const auto Iterator = Requests.find(RequestId);
					if (Iterator == Requests.end()) return false;
					if (IsReady(*Iterator->second)) return true;
				}
				if (TimeoutSeconds >= 0.0 && std::chrono::duration<double>(FClock::now() - StartTime).count() >= TimeoutSeconds) return false;
				std::this_thread::yield();
			}
		}

		auto Start() -> FAssetCompilerStartResult
		{
			std::lock_guard Lock(Mutex);
			if (bAcceptingRequests) return {};
			if (!IsTaskSchedulerRunning()) return {EAssetCompilerStartError::SchedulerUnavailable};
			if (RunningCount != 0 || !InteractiveQueue.empty()
				|| !BackgroundQueue.empty() || PendingRequestCount != 0)
			{
				return {EAssetCompilerStartError::Undrained, PendingRequestCount};
			}
			bShutdown = false;
			Scope = CreateTaskScope();
			if (!Scope.IsValid()) return {EAssetCompilerStartError::TaskScopeUnavailable};
			Attribution = RegisterTaskAttribution("Engine", "Texture2DCompile");
			bAcceptingRequests = true;
			ConsecutiveInteractive = 0;
			return {};
		}

		auto StopAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAcceptingRequests = false;
		}

		auto Shutdown() -> void
		{
			std::vector<std::shared_ptr<FRequestState>> Queued;
			std::vector<std::shared_ptr<FAssetCompilationOperation>> BuildOperations;
			{
				std::lock_guard Lock(Mutex);
				if (bShutdown) return;
				bAcceptingRequests = false;
				bShutdown = true;
				for (const auto& [RequestId, RequestState] : Requests)
				{
					RequestState->bCancellationRequested.store(true, std::memory_order_release);
					if (RequestState->BuildOperation) BuildOperations.push_back(RequestState->BuildOperation);
				}
				while (!InteractiveQueue.empty()) { Queued.push_back(std::move(InteractiveQueue.front())); InteractiveQueue.pop_front(); }
				while (!BackgroundQueue.empty()) { Queued.push_back(std::move(BackgroundQueue.front())); BackgroundQueue.pop_front(); }
			}
			for (const auto& Owner : BuildOperations) Owner->RequestCancel();
			for (const auto& RequestState : Queued)
				CompleteWithoutWorker(RequestState, DerivedData::EStatus::Canceled);
			Scope.Close(ETaskScopeCloseMode::Cancel);
			// Even tasks canceled before their body runs are consumed by the owner pump.
			for (;;)
			{
				Pump(std::numeric_limits<uint32>::max());
				bool bDone;
				{ std::lock_guard Lock(Mutex); bDone = RunningCount == 0; }
				if (bDone) break;
				std::this_thread::yield();
			}
			Pump(std::numeric_limits<uint32>::max());
			const ETaskScopeWaitResult ScopeWait = Scope.WaitFor(5.0);
			if (ScopeWait != ETaskScopeWaitResult::Quiescent)
				DURIN_ERROR_CATEGORY("Texture", "Texture2D compilation scope did not become quiescent during shutdown ({}).", static_cast<uint32>(ScopeWait));
		}

		FTextureCompilingManagerConfig Config;
		FTaskScope Scope;
		FTaskAttribution Attribution;
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::shared_ptr<FRequestState>> Requests;
		std::deque<std::shared_ptr<FRequestState>> InteractiveQueue;
		std::deque<std::shared_ptr<FRequestState>> BackgroundQueue;
		uint32 PendingRequestCount = 0;
		uint64 NextRequestId = 1;
		uint64 InFlightEstimatedBytes = 0;
		uint32 RunningCount = 0;
		uint32 ConsecutiveInteractive = 0;
		bool bAcceptingRequests = false;
		bool bShutdown = false;
		std::function<void(uint64, ETexture2DCompilationPhase)> PhaseHookForTests;
	};

	FTextureCompilingManager::FTextureCompilingManager(
		const FTextureCompilingManagerConfig& Config)
		: QueueState(std::make_shared<FQueueState>(Config))
	{
	}

	FTextureCompilingManager::~FTextureCompilingManager()
	{
		Shutdown();
	}

	auto FTextureCompilingManager::SubmitWork(
		FTexture2DCompilationWork Request,
		FTexture2DCompilationWorkCompletion Completion) -> uint64
	{
		return QueueState ? QueueState->Submit(std::move(Request), std::move(Completion)) : 0;
	}

	auto FTextureCompilingManager::CancelWork(uint64 RequestId) -> bool
	{
		return QueueState && QueueState->Cancel(RequestId);
	}

	auto FTexture2DCompilationProgress::Read() const -> FTexture2DCompilationDiagnostic
	{
		std::lock_guard Lock(Mutex);
		auto Result = Diagnostic;
		if (Result.Phase == ETexture2DCompilationPhase::Queued)
			Result.QueuedNanoseconds = NowNanoseconds() - EnqueueNanoseconds;
		else if (BuildStartNanoseconds != 0
			&& Result.Phase == ETexture2DCompilationPhase::Building)
			Result.Metrics.BuildElapsedNanoseconds = NowNanoseconds() - BuildStartNanoseconds;
		return Result;
	}

	auto FTextureCompilingManager::GetQueuedWorkCount() const -> uint32
	{
		if (!QueueState) return 0;
		std::lock_guard Lock(QueueState->Mutex);
		return static_cast<uint32>(QueueState->InteractiveQueue.size() + QueueState->BackgroundQueue.size());
	}

	auto FTextureCompilingManager::GetRunningWorkCount() const -> uint32
	{
		if (!QueueState) return 0;
		std::lock_guard Lock(QueueState->Mutex);
		return QueueState->RunningCount;
	}

	auto FTextureCompilingManager::SetPhaseHookForTests(
		std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void
	{
		if (!QueueState) return;
		std::lock_guard Lock(QueueState->Mutex);
		QueueState->PhaseHookForTests = std::move(Hook);
	}

	auto FTextureCompilingManager::PumpWorkCompletions(uint32 MaximumCount,
		std::optional<std::chrono::steady_clock::time_point> Deadline, uint64 OnlyRequest) -> uint32
	{
		return QueueState ? QueueState->Pump(MaximumCount, Deadline, OnlyRequest) : 0;
	}

	auto FTextureCompilingManager::StartWorkAdmission() -> FAssetCompilerStartResult
	{
		return QueueState ? QueueState->Start() : FAssetCompilerStartResult{EAssetCompilerStartError::StateUnavailable};
	}

	auto FTextureCompilingManager::StopWorkAdmission() -> void
	{
		if (QueueState) QueueState->StopAdmission();
	}

	auto FTextureCompilingManager::WaitForWork(
		uint64 RequestId, double TimeoutSeconds) -> bool
	{
		return QueueState && QueueState->WaitForRequest(RequestId, TimeoutSeconds);
	}

	auto FTextureCompilingManager::ShutdownWorkQueue() -> void
	{
		if (QueueState) QueueState->Shutdown();
	}

}

#endif

#include "Texture/TextureCompilingManager.h"

#include "Texture/Texture2DBuild.h"

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
			uint64 PixelCount = SaturatingMultiply(Request.EstimatedWidth, Request.EstimatedHeight);
			uint64 WorkingBytes = SaturatingMultiply(PixelCount, 12);
			if (WorkingBytes == 0)
				WorkingBytes = std::max<uint64>(
					SaturatingMultiply(Request.ImportedData.Pixels.GetPayloadSize(), 3),
					64ull * 1024ull * 1024ull);
			return SaturatingAdd(Request.ImportedData.Pixels.GetPayloadSize(), WorkingBytes);
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
			FTexture2DCompilationDiagnostic Diagnostic;
			Tasks::TTask<FTexture2DCompilationWorkResult> Task;
			std::optional<FTexture2DCompilationWorkResult> RejectedResult;
			bool bDelivered = false;
			bool bAdmitted = false;
			std::atomic<bool> bCancellationRequested = false;
			uint64 EstimatedBytes = 0;
			uint64 EnqueueNanoseconds = 0;
			uint64 WorkerStartNanoseconds = 0;
			mutable std::mutex Mutex;
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
			if (!Completion || IsObjectHandleNull(Request.Owner)
				|| Request.AssetIdentity.empty() || !Request.ImportedData.IsValid()
				|| Request.ImportedDataIdentity.IsZero()) return 0;
			auto RequestState = std::make_shared<FRequestState>();
			RequestState->Request = std::move(Request);
			RequestState->Completion = std::move(Completion);
			RequestState->EstimatedBytes = EstimateBuildBytes(RequestState->Request);
			RequestState->EnqueueNanoseconds = NowNanoseconds();
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingRequests || PendingRequestCount >= Config.MaxPendingRequests) return 0;
				RequestState->Diagnostic.RequestId = NextRequestId++;
				++PendingRequestCount;
				RequestState->Diagnostic.RequestSerial = RequestState->Request.RequestSerial;
				RequestState->Diagnostic.AssetIdentity = RequestState->Request.AssetIdentity;
				RequestState->Diagnostic.Phase = ETexture2DCompilationPhase::Queued;
				RequestState->Diagnostic.Metrics.EstimatedBytes = RequestState->EstimatedBytes;
				Requests.emplace(RequestState->Diagnostic.RequestId, RequestState);
				if (RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive)
					InteractiveQueue.push_back(RequestState);
				else BackgroundQueue.push_back(RequestState);
			}
			Admit();
			return RequestState->Diagnostic.RequestId;
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
			return RequestState.bDelivered || RequestState.RejectedResult.has_value()
				|| (RequestState.Task.IsValid() && RequestState.Task.GetCompletion().IsReady());
		}

		auto Admit() -> void
		{
			std::vector<std::shared_ptr<FRequestState>> Admitted;
			std::vector<std::shared_ptr<FRequestState>> Cancelled;
			{
				std::lock_guard Lock(Mutex);
				for (const auto& [Id, RequestState] : Requests)
				{
					if (!RequestState->bAdmitted || !IsReady(*RequestState)) continue;
					RequestState->bAdmitted = false;
					require(RunningCount > 0 && InFlightEstimatedBytes >= RequestState->EstimatedBytes);
					--RunningCount;
					InFlightEstimatedBytes -= RequestState->EstimatedBytes;
				}
				while (!bShutdown && RunningCount < Config.MaxWorkers)
				{
					std::shared_ptr<FRequestState> RequestState = SelectNextRequestLocked();
					if (!RequestState) break;
					if (RequestState->bCancellationRequested.load(std::memory_order_acquire))
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
					RequestState->bAdmitted = true;
					++RunningCount;
					InFlightEstimatedBytes = SaturatingAdd(InFlightEstimatedBytes, RequestState->EstimatedBytes);
					if (RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive)
						++ConsecutiveInteractive;
					else ConsecutiveInteractive = 0;
					Admitted.push_back(std::move(RequestState));
				}
			}
			for (const std::shared_ptr<FRequestState>& RequestState : Cancelled)
				CompleteWithoutWorker(RequestState, ETexture2DCompilationPhase::Cancelled, "Texture build was cancelled before admission.");
			for (const std::shared_ptr<FRequestState>& RequestState : Admitted) Launch(RequestState);
		}

		auto Launch(const std::shared_ptr<FRequestState>& RequestState) -> void
		{
			const std::shared_ptr<FQueueState> Self = shared_from_this();
			Tasks::FTaskGroup Group(Scope.GetToken());
			Tasks::FTaskExecutionOptions Options;
			Options.DebugName = "Texture2D.Build";
			Options.Attribution = Attribution;
			Options.Priority = RequestState->Request.Priority == ETexture2DCompilationPriority::Interactive
				? ETaskPriority::High : ETaskPriority::Low;
			Options.EstimatedResultBytes = SaturatingAdd(RequestState->EstimatedBytes, sizeof(FTexture2DCompilationWorkResult));
			auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options,
				[Self, RequestState](Tasks::FTaskContext& Context) {
					auto Result = Self->RunWorker(RequestState, Context.GetCancellationToken());
					Self->UpdateDiagnostic(RequestState, Result);
					return Result;
				});
			if (!Admission.HasValue())
			{
				CompleteWithoutWorker(RequestState, ETexture2DCompilationPhase::Failed, "Texture build task admission failed.");
				return;
			}
			std::lock_guard Lock(Mutex);
			RequestState->Task = std::move(Admission).TakeValue();
		}

		static auto MakeFailureResult(
			const FRequestState& RequestState,
			ETexture2DCompilationPhase Phase,
			std::string Error) -> FTexture2DCompilationWorkResult
		{
			return {
				.RequestId = RequestState.Diagnostic.RequestId,
				.Owner = RequestState.Request.Owner,
				.RequestSerial = RequestState.Request.RequestSerial,
				.AssetIdentity = RequestState.Request.AssetIdentity,
				.Settings = RequestState.Request.Settings,
				.Error = std::move(Error),
				.Metrics = {.EstimatedBytes = RequestState.EstimatedBytes},
				.Phase = Phase,
				.bSourceDecoderInvoked = RequestState.Request.bSourceDecoderInvoked};
		}

		auto SetPhase(const std::shared_ptr<FRequestState>& RequestState, ETexture2DCompilationPhase Phase) -> void
		{
			{
				std::lock_guard RequestStateLock(RequestState->Mutex);
				RequestState->Diagnostic.Phase = Phase;
			}
			NotifyPhaseHook(RequestState->Diagnostic.RequestId, Phase);
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
			const FTaskCancellationToken& Token) -> FTexture2DCompilationWorkResult
		{
			FTexture2DCompilationWorkResult Result = MakeFailureResult(
				*RequestState, ETexture2DCompilationPhase::Failed, {});
			const uint64 WorkerStart = NowNanoseconds();
			{
				std::lock_guard RequestStateLock(RequestState->Mutex);
				RequestState->WorkerStartNanoseconds = WorkerStart;
				RequestState->Diagnostic.QueuedNanoseconds = WorkerStart - RequestState->EnqueueNanoseconds;
			}
			const auto Cancel = [&] { return IsCancelled(RequestState, Token); };
			if (Cancel())
			{
				Result.Phase = ETexture2DCompilationPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				return Result;
			}

			SetPhase(RequestState, ETexture2DCompilationPhase::Preparing);
			if (Cancel())
			{
				Result.Phase = ETexture2DCompilationPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				return Result;
			}
			const uint64 PreparationStart = NowNanoseconds();
			FTexture2DBuildRequest BuildRequest{
				.ImportedData = std::move(RequestState->Request.ImportedData)};
			Result.Metrics.PreparationNanoseconds = NowNanoseconds() - PreparationStart;
			Result.Metrics.DecodedBytes = 0;
			Result.ImportedDataIdentity = RequestState->Request.ImportedDataIdentity;

			SetPhase(RequestState, ETexture2DCompilationPhase::Building);
			const FTexture2DBuildSettingsSnapshot& Settings = RequestState->Request.Settings;
			FTexture2DBuildMetrics RecipeMetrics;
			bool bEnteredPersisting = false;
			const FTexture2DBuildExecutionControl Control{
				.ShouldCancel = Cancel,
				.OnPersisting = [&] {
					bEnteredPersisting = true;
					SetPhase(RequestState, ETexture2DCompilationPhase::Persisting);
				},
				.Metrics = &RecipeMetrics};
			FTexture2DBuildProduct Product;
			BuildRequest.Settings = {
					.Usage = Settings.Usage,
					.CompressionQuality = Settings.CompressionQuality,
					.AlphaMipMode = Settings.AlphaMipMode,
					.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
					.MaxResolution = Settings.MaxResolution,
					.bSRGB = Settings.bSRGB};
			BuildRequest.TargetPlatform = RequestState->Request.TargetPlatform;
			BuildRequest.TargetProfile = RequestState->Request.TargetProfile;
			BuildRequest.bPersistDerivedData = RequestState->Request.bPersistDerivedData;
			const FTexture2DBuildResult BuildResult = InvokeTexture2DBuildProvider(
				BuildRequest, Product, Result.InputIdentity, &Control);
			if (!BuildResult)
			{
				Result.Error = BuildResult.Diagnostic;
				Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
				Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
				Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
				Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
				Result.Phase = BuildResult.Status == ETexture2DBuildStatus::Cancelled
					? ETexture2DCompilationPhase::Cancelled
					: ETexture2DCompilationPhase::Failed;
				if (Result.Phase == ETexture2DCompilationPhase::Failed)
					Result.FailurePhase = bEnteredPersisting
						? ETexture2DCompilationPhase::Persisting : ETexture2DCompilationPhase::Building;
				return Result;
			}
			Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
			Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
			Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
			Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
			Result.Metrics.ResultBytes = PlatformDataBytes(Product.PlatformData);
			Result.DerivedDataKey = std::move(Product.DerivedDataKey);
			Result.PersistenceDiagnostic = std::move(Product.PersistenceDiagnostic);
			Result.Origin = Product.Origin;
			Result.bSourceDecoderInvoked = RequestState->Request.bSourceDecoderInvoked;
			if (Product.Origin == ETexture2DBuildProductOrigin::Rebuilt)
				Result.Metrics.DecodedBytes = BuildRequest.ImportedData.Pixels.GetPayloadSize();
			Result.ImportedData = std::make_unique<FTexture2DImportedData>(
				std::move(BuildRequest.ImportedData));
			Result.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData));
			Result.Error.clear();
			Result.Phase = Cancel() ? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::UploadPending;
			Result.Metrics.WorkerNanoseconds = NowNanoseconds() - WorkerStart;
			return Result;
		}

		auto CompleteWithoutWorker(
			const std::shared_ptr<FRequestState>& RequestState,
			ETexture2DCompilationPhase Phase,
			std::string Error) -> void
		{
			auto Result = MakeFailureResult(*RequestState, Phase, std::move(Error));
			UpdateDiagnostic(RequestState, Result);
			std::lock_guard Lock(Mutex);
			RequestState->RejectedResult.emplace(std::move(Result));
		}

		auto UpdateDiagnostic(
			const std::shared_ptr<FRequestState>& RequestState,
			const FTexture2DCompilationWorkResult& Result) -> void
		{
			{
				std::lock_guard RequestStateLock(RequestState->Mutex);
				RequestState->Diagnostic.Phase = Result.Phase;
				RequestState->Diagnostic.Message = Result.Error;
				RequestState->Diagnostic.DerivedDataKey = Result.DerivedDataKey.ToString();
				RequestState->Diagnostic.Metrics = Result.Metrics;
				RequestState->Diagnostic.FailurePhase = Result.FailurePhase;
				RequestState->Diagnostic.Origin = Result.Origin == ETexture2DBuildProductOrigin::CacheHit
					? ETexture2DCompilationOrigin::CacheHit
					: ETexture2DCompilationOrigin::Rebuilt;
				RequestState->Diagnostic.bSourceDecoderInvoked = Result.bSourceDecoderInvoked;
				RequestState->Diagnostic.QueuedNanoseconds = RequestState->WorkerStartNanoseconds != 0
					? RequestState->WorkerStartNanoseconds - RequestState->EnqueueNanoseconds
					: NowNanoseconds() - RequestState->EnqueueNanoseconds;
				RequestState->Diagnostic.WorkerNanoseconds = Result.Metrics.WorkerNanoseconds;
			}
			NotifyPhaseHook(RequestState->Diagnostic.RequestId, Result.Phase);
		}

		auto Cancel(uint64 RequestId) -> bool
		{
			std::lock_guard Lock(Mutex);
			const auto Iterator = Requests.find(RequestId);
			if (Iterator == Requests.end() || Iterator->second->bDelivered) return false;
			return !Iterator->second->bCancellationRequested.exchange(true, std::memory_order_acq_rel);
		}

		auto Pump(uint32 MaximumCount) -> uint32
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			const auto Self = shared_from_this();
			uint32 Count = 0;
			while (Count < MaximumCount)
			{
				Admit();
				std::shared_ptr<FRequestState> Ready;
				{
					std::lock_guard Lock(Mutex);
					for (const auto& [Id, RequestState] : Requests)
					{
						if (!RequestState->bDelivered && IsReady(*RequestState)) { Ready = RequestState; break; }
					}
					if (!Ready) break;
					// Detach before invoking callbacks, which may submit or shut down the manager.
					Ready->bDelivered = true;
				}
				auto Result = [&]() -> FTexture2DCompilationWorkResult {
					if (Ready->RejectedResult) return std::move(*Ready->RejectedResult);
					if (Ready->Task.GetState() == ETaskState::Succeeded) return std::move(Ready->Task).TakeResult();
					const bool bCanceled = Ready->Task.GetState() == ETaskState::Canceled
						|| Ready->bCancellationRequested.load();
					return MakeFailureResult(*Ready, bCanceled ? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::Failed,
						bCanceled ? "Texture build was cancelled." : "Texture build task failed.");
				}();
				if (Ready->bCancellationRequested.load())
				{
					Result.Phase = ETexture2DCompilationPhase::Cancelled;
					Result.Error = "Texture build was cancelled.";
				}
				UpdateDiagnostic(Ready, Result);
				const uint64 CompletionStart = NowNanoseconds();
				auto Completion = std::move(Ready->Completion);
				try { if (Completion) Completion(std::move(Result)); }
				catch (...) { DURIN_ERROR_CATEGORY("Texture", "Texture2D compilation completion callback threw an exception."); }
				{
					std::lock_guard RequestLock(Ready->Mutex);
					Ready->Diagnostic.Metrics.CompletionNanoseconds = NowNanoseconds() - CompletionStart;
				}
				{
					std::lock_guard Lock(Mutex);
					Ready->Task = {};
					Ready->RejectedResult.reset();
					Ready->Request.ImportedData = {};
					require(PendingRequestCount > 0);
					--PendingRequestCount;
					CompletedOrder.push_back(Ready->Diagnostic.RequestId);
					while (CompletedOrder.size() > MaximumRetainedDiagnostics)
					{
						Requests.erase(CompletedOrder.front());
						CompletedOrder.pop_front();
					}
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

		auto Start() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (bAcceptingRequests) return true;
			if (!IsTaskSchedulerRunning()) return false;
			if (RunningCount != 0 || !InteractiveQueue.empty()
				|| !BackgroundQueue.empty() || PendingRequestCount != 0)
			{
				return false;
			}
			bShutdown = false;
			Scope = CreateTaskScope();
			if (!Scope.IsValid()) return false;
			Attribution = RegisterTaskAttribution("Engine", "Texture2DCompile");
			bAcceptingRequests = true;
			ConsecutiveInteractive = 0;
			return true;
		}

		auto StopAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAcceptingRequests = false;
		}

		auto Shutdown() -> void
		{
			std::vector<std::shared_ptr<FRequestState>> Queued;
			{
				std::lock_guard Lock(Mutex);
				if (bShutdown) return;
				bAcceptingRequests = false;
				bShutdown = true;
				for (const auto& [RequestId, RequestState] : Requests) RequestState->bCancellationRequested.store(true, std::memory_order_release);
				while (!InteractiveQueue.empty()) { Queued.push_back(std::move(InteractiveQueue.front())); InteractiveQueue.pop_front(); }
				while (!BackgroundQueue.empty()) { Queued.push_back(std::move(BackgroundQueue.front())); BackgroundQueue.pop_front(); }
			}
			for (const auto& RequestState : Queued)
				CompleteWithoutWorker(RequestState, ETexture2DCompilationPhase::Cancelled, "Texture build was cancelled during shutdown.");
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
		std::deque<uint64> CompletedOrder;
		static constexpr size_t MaximumRetainedDiagnostics = 256;
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

	auto FTextureCompilingManager::GetWorkDiagnostic(uint64 RequestId) const -> FTexture2DCompilationDiagnostic
	{
		if (!QueueState) return {};
		std::shared_ptr<FQueueState::FRequestState> RequestState;
		{
			std::lock_guard Lock(QueueState->Mutex);
			const auto Iterator = QueueState->Requests.find(RequestId);
			if (Iterator == QueueState->Requests.end()) return {};
			RequestState = Iterator->second;
		}
		std::lock_guard RequestStateLock(RequestState->Mutex);
		FTexture2DCompilationDiagnostic Result = RequestState->Diagnostic;
		if (Result.Phase == ETexture2DCompilationPhase::Queued)
			Result.QueuedNanoseconds = NowNanoseconds() - RequestState->EnqueueNanoseconds;
		else if (RequestState->WorkerStartNanoseconds != 0
			&& Result.Phase != ETexture2DCompilationPhase::UploadPending
			&& Result.Phase != ETexture2DCompilationPhase::Ready
			&& Result.Phase != ETexture2DCompilationPhase::Failed
			&& Result.Phase != ETexture2DCompilationPhase::Cancelled)
			Result.WorkerNanoseconds = NowNanoseconds() - RequestState->WorkerStartNanoseconds;
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

	auto FTextureCompilingManager::GetWorkManagerDiagnostics() const
		-> FTexture2DCompilationManagerDiagnostics
	{
		FTexture2DCompilationManagerDiagnostics Result;
		if (!QueueState) return Result;
		std::lock_guard Lock(QueueState->Mutex);
		Result.RetainedWorkCount = QueueState->Requests.size();
		Result.InFlightEstimatedBytes = QueueState->InFlightEstimatedBytes;
		Result.QueuedWorkCount = static_cast<uint32>(
			QueueState->InteractiveQueue.size() + QueueState->BackgroundQueue.size());
		Result.RunningWorkCount = QueueState->RunningCount;
		Result.PendingCompletionCount = static_cast<uint32>(std::ranges::count_if(QueueState->Requests,
			[](const auto& Entry) { return !Entry.second->bDelivered && FQueueState::IsReady(*Entry.second); }));
		return Result;
	}

	auto FTextureCompilingManager::SetPhaseHookForTests(
		std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void
	{
		if (!QueueState) return;
		std::lock_guard Lock(QueueState->Mutex);
		QueueState->PhaseHookForTests = std::move(Hook);
	}

	auto FTextureCompilingManager::PumpWorkCompletions(uint32 MaximumCount) -> uint32
	{
		return QueueState ? QueueState->Pump(MaximumCount) : 0;
	}

	auto FTextureCompilingManager::StartWorkAdmission() -> bool
	{
		return QueueState && QueueState->Start();
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

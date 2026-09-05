#include "Texture/TextureCompilingManager.h"

#include "Texture/Texture2DBuildProvider.h"

#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

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
		struct FJob
		{
			FTexture2DCompilationWork Request;
			FTexture2DCompilationWorkCompletion Completion;
			FTexture2DCompilationDiagnostic Diagnostic;
			FTaskHandle Task;
			std::atomic<bool> bCancellationRequested = false;
			std::atomic<bool> bWorkerCompleted = false;
			bool bCompletionQueued = false;
			uint64 EstimatedBytes = 0;
			uint64 EnqueueNanoseconds = 0;
			uint64 WorkerStartNanoseconds = 0;
			mutable std::mutex Mutex;
			std::condition_variable CompletionCondition;
		};

		struct FCompletion
		{
			std::shared_ptr<FJob> Job;
			FTexture2DCompilationWorkResult Result;
		};

		explicit FQueueState(FTextureCompilingManagerConfig InConfig)
			: Config(InConfig)
		{
			Config.MaxWorkers = std::max(Config.MaxWorkers, 1u);
			Config.InteractiveBurstLimit = std::max(Config.InteractiveBurstLimit, 1u);
			Config.InFlightByteBudget = std::max<uint64>(Config.InFlightByteBudget, 1);
		}

		auto Submit(FTexture2DCompilationWork Request, FTexture2DCompilationWorkCompletion Completion) -> uint64
		{
			if (!Completion || IsObjectHandleNull(Request.Owner)
				|| Request.AssetIdentity.empty() || !Request.ImportedData.IsValid()
				|| Request.ImportedDataIdentity.IsZero()) return 0;
			auto Job = std::make_shared<FJob>();
			Job->Request = std::move(Request);
			Job->Completion = std::move(Completion);
			Job->EstimatedBytes = EstimateBuildBytes(Job->Request);
			Job->EnqueueNanoseconds = NowNanoseconds();
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingRequests) return 0;
				Job->Diagnostic.RequestId = NextRequestId++;
				Job->Diagnostic.RequestSerial = Job->Request.RequestSerial;
				Job->Diagnostic.AssetIdentity = Job->Request.AssetIdentity;
				Job->Diagnostic.Phase = ETexture2DCompilationPhase::Queued;
				Job->Diagnostic.Metrics.EstimatedBytes = Job->EstimatedBytes;
				Jobs.emplace(Job->Diagnostic.RequestId, Job);
				if (Job->Request.Priority == ETexture2DCompilationPriority::Interactive)
					InteractiveQueue.push_back(Job);
				else BackgroundQueue.push_back(Job);
			}
			Admit();
			return Job->Diagnostic.RequestId;
		}

		auto SelectNextJobLocked() -> std::shared_ptr<FJob>
		{
			auto PopAvailable = [](std::deque<std::shared_ptr<FJob>>& Queue) {
				while (!Queue.empty())
				{
					std::shared_ptr<FJob> Job = std::move(Queue.front());
					Queue.pop_front();
					if (Job) return Job;
				}
				return std::shared_ptr<FJob>{};
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

		auto Admit() -> void
		{
			std::vector<std::shared_ptr<FJob>> Admitted;
			std::vector<std::shared_ptr<FJob>> Cancelled;
			{
				std::lock_guard Lock(Mutex);
				while (!bShutdown && RunningCount < Config.MaxWorkers)
				{
					std::shared_ptr<FJob> Job = SelectNextJobLocked();
					if (!Job) break;
					if (Job->bCancellationRequested.load(std::memory_order_acquire))
					{
						Cancelled.push_back(std::move(Job));
						continue;
					}
					const bool bMayRunAlone = RunningCount == 0 && InFlightEstimatedBytes == 0;
					if (!bMayRunAlone
						&& Job->EstimatedBytes > Config.InFlightByteBudget - std::min(
							InFlightEstimatedBytes, Config.InFlightByteBudget))
					{
						if (Job->Request.Priority == ETexture2DCompilationPriority::Interactive)
							InteractiveQueue.push_front(std::move(Job));
						else BackgroundQueue.push_front(std::move(Job));
						break;
					}
					++RunningCount;
					InFlightEstimatedBytes = SaturatingAdd(InFlightEstimatedBytes, Job->EstimatedBytes);
					if (Job->Request.Priority == ETexture2DCompilationPriority::Interactive)
						++ConsecutiveInteractive;
					else ConsecutiveInteractive = 0;
					Admitted.push_back(std::move(Job));
				}
			}
			for (const std::shared_ptr<FJob>& Job : Cancelled)
				CompleteWithoutWorker(Job, ETexture2DCompilationPhase::Cancelled, "Texture build was cancelled before admission.");
			for (const std::shared_ptr<FJob>& Job : Admitted) Launch(Job);
		}

		auto Launch(const std::shared_ptr<FJob>& Job) -> void
		{
			const std::shared_ptr<FQueueState> Self = shared_from_this();
			FTaskLaunchOptions Options;
			Options.Attribution = Attribution;
			Options.Scope = Scope.GetToken();
			FTaskHandle Task = LaunchCancelableTask(
				"Texture2D.Build",
				[Self, Job](const FTaskCancellationToken& Token) {
					Self->RunWorker(Job, Token);
				}, Options);
			if (!Task.IsValid())
			{
				FTexture2DCompilationWorkResult Result = MakeFailureResult(
					*Job, ETexture2DCompilationPhase::Failed,
					"Texture build task admission was rejected.");
				Result.FailurePhase = ETexture2DCompilationPhase::Queued;
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			std::lock_guard JobLock(Job->Mutex);
			if (!Job->bWorkerCompleted.load(std::memory_order_acquire)) Job->Task = std::move(Task);
		}

		static auto MakeFailureResult(
			const FJob& Job,
			ETexture2DCompilationPhase Phase,
			std::string Error) -> FTexture2DCompilationWorkResult
		{
			return {
				.RequestId = Job.Diagnostic.RequestId,
				.Owner = Job.Request.Owner,
				.RequestSerial = Job.Request.RequestSerial,
				.AssetIdentity = Job.Request.AssetIdentity,
				.Settings = Job.Request.Settings,
				.Error = std::move(Error),
				.Metrics = {.EstimatedBytes = Job.EstimatedBytes},
				.Phase = Phase,
				.bSourceDecoderInvoked = Job.Request.bSourceDecoderInvoked};
		}

		auto SetPhase(const std::shared_ptr<FJob>& Job, ETexture2DCompilationPhase Phase) -> void
		{
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->Diagnostic.Phase = Phase;
			}
			NotifyPhaseHook(Job->Diagnostic.RequestId, Phase);
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
			const std::shared_ptr<FJob>& Job,
			const FTaskCancellationToken& Token) const -> bool
		{
			return Job->bCancellationRequested.load(std::memory_order_acquire)
				|| Token.IsCancellationRequested();
		}

		auto RunWorker(
			const std::shared_ptr<FJob>& Job,
			const FTaskCancellationToken& Token) -> void
		{
			FTexture2DCompilationWorkResult Result = MakeFailureResult(
				*Job, ETexture2DCompilationPhase::Failed, {});
			const uint64 WorkerStart = NowNanoseconds();
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->WorkerStartNanoseconds = WorkerStart;
				Job->Diagnostic.QueuedNanoseconds = WorkerStart - Job->EnqueueNanoseconds;
			}
			const auto Cancel = [&] { return IsCancelled(Job, Token); };
			if (Cancel())
			{
				Result.Phase = ETexture2DCompilationPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				CompleteAdmitted(Job, std::move(Result));
				return;
			}

			SetPhase(Job, ETexture2DCompilationPhase::Preparing);
			if (Cancel())
			{
				Result.Phase = ETexture2DCompilationPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			const uint64 PreparationStart = NowNanoseconds();
			FTexture2DBuildRequest BuildRequest{
				.ImportedData = std::move(Job->Request.ImportedData)};
			Result.Metrics.PreparationNanoseconds = NowNanoseconds() - PreparationStart;
			Result.Metrics.DecodedBytes = 0;
			Result.ImportedDataIdentity = Job->Request.ImportedDataIdentity;
			Result.CapturedGeneration = Job->Request.CapturedGeneration;

			SetPhase(Job, ETexture2DCompilationPhase::Building);
			const FTexture2DBuildSettingsSnapshot& Settings = Job->Request.Settings;
			FTexture2DRecipeMetrics RecipeMetrics;
			bool bEnteredPersisting = false;
			const FTexture2DBuildExecutionControl Control{
				.ShouldCancel = Cancel,
				.OnPersisting = [&] {
					bEnteredPersisting = true;
					SetPhase(Job, ETexture2DCompilationPhase::Persisting);
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
			BuildRequest.TargetPlatform = Job->Request.TargetPlatform;
			BuildRequest.TargetProfile = Job->Request.TargetProfile;
			BuildRequest.bPersistDerivedData = Job->Request.bPersistDerivedData;
			if (!InvokeTexture2DBuildProvider(BuildRequest,
				Product, Result.InputIdentity, Result.Error, &Control))
			{
				Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
				Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
				Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
				Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
				Result.Phase = Cancel() ? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::Failed;
				if (Result.Phase == ETexture2DCompilationPhase::Failed)
					Result.FailurePhase = bEnteredPersisting
						? ETexture2DCompilationPhase::Persisting : ETexture2DCompilationPhase::Building;
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
			Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
			Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
			Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
			Result.Metrics.ResultBytes = PlatformDataBytes(Product.PlatformData);
			Result.DerivedDataKey = std::move(Product.DerivedDataKey);
			Result.PersistenceDiagnostic = std::move(Product.PersistenceDiagnostic);
			Result.Origin = Product.Origin;
			Result.bSourceDecoderInvoked = Job->Request.bSourceDecoderInvoked;
			if (Product.Origin == ETexture2DBuildProductOrigin::Rebuilt)
				Result.Metrics.DecodedBytes = BuildRequest.ImportedData.Pixels.GetPayloadSize();
			Result.ImportedData = std::make_unique<FTexture2DImportedData>(
				std::move(BuildRequest.ImportedData));
			Result.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData));
			Result.Error.clear();
			Result.Phase = Cancel() ? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::UploadPending;
			Result.Metrics.WorkerNanoseconds = NowNanoseconds() - WorkerStart;
			CompleteAdmitted(Job, std::move(Result));
		}

		auto CompleteWithoutWorker(
			const std::shared_ptr<FJob>& Job,
			ETexture2DCompilationPhase Phase,
			std::string Error) -> void
		{
			FTexture2DCompilationWorkResult Result = MakeFailureResult(*Job, Phase, std::move(Error));
			PushCompletion(Job, std::move(Result));
		}

		auto CompleteAdmitted(
			const std::shared_ptr<FJob>& Job,
			FTexture2DCompilationWorkResult Result) -> void
		{
			Job->bWorkerCompleted.store(true, std::memory_order_release);
			{
				std::lock_guard Lock(Mutex);
				check(RunningCount > 0);
				--RunningCount;
				InFlightEstimatedBytes -= std::min(InFlightEstimatedBytes, Job->EstimatedBytes);
			}
			PushCompletion(Job, std::move(Result));
			Admit();
		}

		auto PushCompletion(
			const std::shared_ptr<FJob>& Job,
			FTexture2DCompilationWorkResult Result) -> void
		{
			if (Result.Metrics.WorkerNanoseconds == 0)
				Result.Metrics.WorkerNanoseconds = NowNanoseconds() - Job->EnqueueNanoseconds;
			Result.Metrics.EstimatedBytes = Job->EstimatedBytes;
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->Diagnostic.Phase = Result.Phase;
				Job->Diagnostic.Message = Result.Error;
				Job->Diagnostic.DerivedDataKey = Result.DerivedDataKey;
				Job->Diagnostic.Metrics = Result.Metrics;
				Job->Diagnostic.FailurePhase = Result.FailurePhase;
				Job->Diagnostic.Origin = Result.Origin == ETexture2DBuildProductOrigin::CacheHit
					? ETexture2DCompilationOrigin::CacheHit
					: ETexture2DCompilationOrigin::Rebuilt;
				Job->Diagnostic.bSourceDecoderInvoked = Result.bSourceDecoderInvoked;
				Job->Diagnostic.QueuedNanoseconds = Job->WorkerStartNanoseconds != 0
					? Job->WorkerStartNanoseconds - Job->EnqueueNanoseconds
					: NowNanoseconds() - Job->EnqueueNanoseconds;
				Job->Diagnostic.WorkerNanoseconds = Result.Metrics.WorkerNanoseconds;
				Job->bCompletionQueued = true;
			}
			const ETexture2DCompilationPhase FinalPhase = Result.Phase;
			{
				std::lock_guard Lock(Mutex);
				Completions.push_back({Job, std::move(Result)});
			}
			Job->CompletionCondition.notify_all();
			NotifyPhaseHook(Job->Diagnostic.RequestId, FinalPhase);
		}

		auto Cancel(uint64 RequestId) -> bool
		{
			std::shared_ptr<FJob> Job;
			{
				std::lock_guard Lock(Mutex);
				const auto Iterator = Jobs.find(RequestId);
				if (Iterator == Jobs.end()) return false;
				Job = Iterator->second;
			}
			{
				std::lock_guard JobLock(Job->Mutex);
				if (Job->bCompletionQueued) return false;
			}
			return !Job->bCancellationRequested.exchange(true, std::memory_order_acq_rel);
		}

		auto Pump(uint32 MaximumCount) -> uint32
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			uint32 Count = 0;
			while (Count < MaximumCount)
			{
				FCompletion Entry;
				{
					std::lock_guard Lock(Mutex);
					if (Completions.empty()) break;
					Entry = std::move(Completions.front());
					Completions.pop_front();
				}
				const auto CompletionStart = std::chrono::steady_clock::now();
				if (Entry.Job->Completion) Entry.Job->Completion(std::move(Entry.Result));
				const uint64 CompletionNanoseconds = static_cast<uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - CompletionStart).count());
				Entry.Job->Completion = {};
				{
					std::lock_guard JobLock(Entry.Job->Mutex);
					Entry.Job->Diagnostic.Metrics.CompletionNanoseconds =
						CompletionNanoseconds;
				}
				{
					std::lock_guard Lock(Mutex);
					CompletedOrder.push_back(Entry.Job->Diagnostic.RequestId);
					while (CompletedOrder.size() > MaximumRetainedDiagnostics)
					{
						Jobs.erase(CompletedOrder.front());
						CompletedOrder.pop_front();
					}
				}
				++Count;
			}
			return Count;
		}

		auto WaitForRequest(uint64 RequestId, double TimeoutSeconds) -> bool
		{
			std::shared_ptr<FJob> Job;
			{
				std::lock_guard Lock(Mutex);
				const auto Iterator = Jobs.find(RequestId);
				if (Iterator == Jobs.end()) return false;
				Job = Iterator->second;
			}
			std::unique_lock JobLock(Job->Mutex);
			if (TimeoutSeconds < 0.0)
			{
				Job->CompletionCondition.wait(JobLock, [&] { return Job->bCompletionQueued; });
				return true;
			}
			return Job->CompletionCondition.wait_for(
				JobLock,
				std::chrono::duration<double>(TimeoutSeconds),
				[&] { return Job->bCompletionQueued; });
		}

		auto Start() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (bAcceptingRequests) return true;
			if (!IsTaskSchedulerRunning()) return false;
			if (RunningCount != 0 || !InteractiveQueue.empty()
				|| !BackgroundQueue.empty() || !Completions.empty())
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
			std::vector<FTaskHandle> Tasks;
			std::vector<std::shared_ptr<FJob>> Queued;
			{
				std::lock_guard Lock(Mutex);
				if (bShutdown) return;
				bAcceptingRequests = false;
				bShutdown = true;
				for (const auto& [RequestId, Job] : Jobs)
				{
					(void)RequestId;
					Job->bCancellationRequested.store(true, std::memory_order_release);
					std::lock_guard JobLock(Job->Mutex);
					if (Job->Task.IsValid() && !Job->bWorkerCompleted.load(std::memory_order_acquire))
						Tasks.push_back(Job->Task);
				}
				while (!InteractiveQueue.empty())
				{
					Queued.push_back(std::move(InteractiveQueue.front()));
					InteractiveQueue.pop_front();
				}
				while (!BackgroundQueue.empty())
				{
					Queued.push_back(std::move(BackgroundQueue.front()));
					BackgroundQueue.pop_front();
				}
			}
			for (const std::shared_ptr<FJob>& Job : Queued)
				CompleteWithoutWorker(Job, ETexture2DCompilationPhase::Cancelled, "Texture build was cancelled during shutdown.");
			Scope.Close(ETaskScopeCloseMode::Cancel);
			if (!Tasks.empty()) WaitAll(Tasks);
			const ETaskScopeWaitResult ScopeWait = Scope.WaitFor(5.0);
			if (ScopeWait != ETaskScopeWaitResult::Quiescent)
				DURIN_ERROR_CATEGORY("Texture", "Texture2D compilation scope did not become quiescent during shutdown ({}).",
					static_cast<uint32>(ScopeWait));
			Pump(std::numeric_limits<uint32>::max());
		}

		FTextureCompilingManagerConfig Config;
		FTaskScope Scope;
		FTaskAttribution Attribution;
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::shared_ptr<FJob>> Jobs;
		std::deque<std::shared_ptr<FJob>> InteractiveQueue;
		std::deque<std::shared_ptr<FJob>> BackgroundQueue;
		std::deque<FCompletion> Completions;
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
		std::shared_ptr<FQueueState::FJob> Job;
		{
			std::lock_guard Lock(QueueState->Mutex);
			const auto Iterator = QueueState->Jobs.find(RequestId);
			if (Iterator == QueueState->Jobs.end()) return {};
			Job = Iterator->second;
		}
		std::lock_guard JobLock(Job->Mutex);
		FTexture2DCompilationDiagnostic Result = Job->Diagnostic;
		if (Result.Phase == ETexture2DCompilationPhase::Queued)
			Result.QueuedNanoseconds = NowNanoseconds() - Job->EnqueueNanoseconds;
		else if (Job->WorkerStartNanoseconds != 0
			&& Result.Phase != ETexture2DCompilationPhase::UploadPending
			&& Result.Phase != ETexture2DCompilationPhase::Ready
			&& Result.Phase != ETexture2DCompilationPhase::Failed
			&& Result.Phase != ETexture2DCompilationPhase::Cancelled)
			Result.WorkerNanoseconds = NowNanoseconds() - Job->WorkerStartNanoseconds;
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
		Result.RetainedWorkCount = QueueState->Jobs.size();
		Result.InFlightEstimatedBytes = QueueState->InFlightEstimatedBytes;
		Result.QueuedWorkCount = static_cast<uint32>(
			QueueState->InteractiveQueue.size() + QueueState->BackgroundQueue.size());
		Result.RunningWorkCount = QueueState->RunningCount;
		Result.PendingCompletionCount = static_cast<uint32>(QueueState->Completions.size());
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

#include "Texture/TextureCompilingManager.h"

#include "Texture/Texture2DBuildProvider.h"

#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskOperation.h"

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
		using FOperations = Tasks::TTaskOperationQueue<FTexture2DCompilationWorkResult>;

		struct FJob
		{
			FTexture2DCompilationWork Request;
			FTexture2DCompilationWorkCompletion Completion;
			FTexture2DCompilationDiagnostic Diagnostic;
			std::optional<FOperations::FTicket> Ticket;
			bool bAdmitted = false;
			std::atomic<bool> bCancellationRequested = false;
			uint64 EstimatedBytes = 0;
			uint64 EnqueueNanoseconds = 0;
			uint64 WorkerStartNanoseconds = 0;
			mutable std::mutex Mutex;
			std::condition_variable CompletionCondition;
		};

		explicit FQueueState(FTextureCompilingManagerConfig InConfig)
			: Config(InConfig)
		{
			Config.MaxWorkers = std::max(Config.MaxWorkers, 1u);
			Config.MaxPendingOperations = std::max(Config.MaxPendingOperations, 1u);
			Config.RetainedResultByteBudget = std::max<uint64>(Config.RetainedResultByteBudget, sizeof(FTexture2DCompilationWorkResult));
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
			if (!Operations) return 0;
			Job->Diagnostic.RequestId = NextRequestId++;
			auto Reservation = Operations->TryReserve(Job->Diagnostic.RequestId, 0,
				SaturatingAdd(Job->EstimatedBytes, sizeof(FTexture2DCompilationWorkResult)),
				[WeakSelf = weak_from_this(), WeakJob = std::weak_ptr<FJob>(Job)](ETaskState Terminal) {
					if (auto Self = WeakSelf.lock()) if (auto Job = WeakJob.lock()) Self->ProducerReady(Job, Terminal);
				});
			if (!Reservation.HasValue()) return 0;
			Job->Ticket.emplace(std::move(Reservation).TakeValue());
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingRequests) return 0;
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
					Job->bAdmitted = true;
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
			Tasks::FTaskGroup Group(Scope.GetToken());
			Tasks::FTaskExecutionOptions Options;
			Options.DebugName = "Texture2D.Build";
			Options.Attribution = Attribution;
			Options.Priority = Job->Request.Priority == ETexture2DCompilationPriority::Interactive
				? ETaskPriority::High : ETaskPriority::Low;
			Options.EstimatedResultBytes = SaturatingAdd(Job->EstimatedBytes, sizeof(FTexture2DCompilationWorkResult));
			auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options,
				[Self, Job](Tasks::FTaskContext& Context) {
					auto Result = Self->RunWorker(Job, Context.GetCancellationToken());
					Self->UpdateDiagnostic(Job, Result);
					return Result;
				});
			if (!Admission.HasValue())
			{
				Job->Ticket->FailAdmission(Admission.GetError());
				return;
			}
			auto Task = std::move(Admission).TakeValue();
			Job->Ticket->Bind(std::move(Task));
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
			const FTaskCancellationToken& Token) -> FTexture2DCompilationWorkResult
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
				return Result;
			}

			SetPhase(Job, ETexture2DCompilationPhase::Preparing);
			if (Cancel())
			{
				Result.Phase = ETexture2DCompilationPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				return Result;
			}
			const uint64 PreparationStart = NowNanoseconds();
			FTexture2DBuildRequest BuildRequest{
				.ImportedData = std::move(Job->Request.ImportedData)};
			Result.Metrics.PreparationNanoseconds = NowNanoseconds() - PreparationStart;
			Result.Metrics.DecodedBytes = 0;
			Result.ImportedDataIdentity = Job->Request.ImportedDataIdentity;

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
			Result.bSourceDecoderInvoked = Job->Request.bSourceDecoderInvoked;
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
			const std::shared_ptr<FJob>& Job,
			ETexture2DCompilationPhase Phase,
			std::string Error) -> void
		{
			UpdateDiagnostic(Job, MakeFailureResult(*Job, Phase, std::move(Error)));
			Job->Ticket->FailAdmission({Tasks::ETaskAdmissionErrorCode::GroupClosed});
		}

		auto ProducerReady(const std::shared_ptr<FJob>& Job, ETaskState) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				if (Job->bAdmitted)
				{
					Job->bAdmitted = false;
					check(RunningCount > 0);
					--RunningCount;
					InFlightEstimatedBytes -= std::min(InFlightEstimatedBytes, Job->EstimatedBytes);
				}
			}
			// Pair with the wait lock; the predicate is Core's reserved ticket state.
			{ std::lock_guard JobLock(Job->Mutex); }
			Job->CompletionCondition.notify_all();
			Admit();
		}

		auto UpdateDiagnostic(
			const std::shared_ptr<FJob>& Job,
			const FTexture2DCompilationWorkResult& Result) -> void
		{
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->Diagnostic.Phase = Result.Phase;
				Job->Diagnostic.Message = Result.Error;
				Job->Diagnostic.DerivedDataKey = Result.DerivedDataKey.ToString();
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
			}
			NotifyPhaseHook(Job->Diagnostic.RequestId, Result.Phase);
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
				if ((!Job->Ticket || Job->Ticket->IsProducerReady())) return false;
			}
			return !Job->bCancellationRequested.exchange(true, std::memory_order_acq_rel);
		}

		auto Pump(uint32 MaximumCount) -> uint32
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			if (!Operations) return 0;
			const auto Self = shared_from_this();
			return Operations->PumpOutcomes(0, [Self](uint64 RequestId, Tasks::TTaskOutcome<FTexture2DCompilationWorkResult> Outcome) {
				std::shared_ptr<FJob> Job;
				{ std::lock_guard Lock(Self->Mutex); Job = Self->Jobs.at(RequestId); }
				auto Result = [&]() -> FTexture2DCompilationWorkResult {
					if (auto* Value = std::get_if<0>(&Outcome)) return std::move(*Value);
					const bool bCanceled = std::holds_alternative<Tasks::FTaskCanceled>(Outcome) || Job->bCancellationRequested.load();
					return MakeFailureResult(*Job, bCanceled ? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::Failed,
						bCanceled ? "Texture build was cancelled." : "Texture build task failed or was rejected.");
				}();
				if (Outcome.index() != 0) Self->UpdateDiagnostic(Job, Result);
				const uint64 CompletionStart = NowNanoseconds();
				auto Completion = std::move(Job->Completion);
				{ std::lock_guard JobLock(Job->Mutex); Job->Ticket.reset(); }
				if (Completion) Completion(std::move(Result));
				{
					std::lock_guard JobLock(Job->Mutex);
					Job->Diagnostic.Metrics.CompletionNanoseconds = NowNanoseconds() - CompletionStart;
				}
				{
					std::lock_guard Lock(Self->Mutex);
					Self->CompletedOrder.push_back(RequestId);
					while (Self->CompletedOrder.size() > MaximumRetainedDiagnostics)
					{
						Self->Jobs.erase(Self->CompletedOrder.front());
						Self->CompletedOrder.pop_front();
					}
				}
			}, MaximumCount);
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
				Job->CompletionCondition.wait(JobLock, [&] { return (!Job->Ticket || Job->Ticket->IsProducerReady()); });
				return true;
			}
			return Job->CompletionCondition.wait_for(
				JobLock,
				std::chrono::duration<double>(TimeoutSeconds),
				[&] { return (!Job->Ticket || Job->Ticket->IsProducerReady()); });
		}

		auto Start() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (bAcceptingRequests) return true;
			if (!IsTaskSchedulerRunning()) return false;
			if (RunningCount != 0 || !InteractiveQueue.empty()
				|| !BackgroundQueue.empty() || (Operations && Operations->GetActiveCount() != 0))
			{
				return false;
			}
			bShutdown = false;
			Scope = CreateTaskScope();
			if (!Scope.IsValid()) return false;
			Attribution = RegisterTaskAttribution("Engine", "Texture2DCompile");
			Tasks::FTaskGroup Group(Scope.GetToken());
			Operations = std::make_unique<FOperations>(Group,
				Tasks::FTaskOperationLimits{Config.MaxPendingOperations, Config.RetainedResultByteBudget}, true);
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
			std::vector<std::shared_ptr<FJob>> Queued;
			{
				std::lock_guard Lock(Mutex);
				if (bShutdown) return;
				bAcceptingRequests = false;
				bShutdown = true;
				for (const auto& [RequestId, Job] : Jobs) Job->bCancellationRequested.store(true, std::memory_order_release);
				while (!InteractiveQueue.empty()) { Queued.push_back(std::move(InteractiveQueue.front())); InteractiveQueue.pop_front(); }
				while (!BackgroundQueue.empty()) { Queued.push_back(std::move(BackgroundQueue.front())); BackgroundQueue.pop_front(); }
			}
			for (const auto& Job : Queued)
				CompleteWithoutWorker(Job, ETexture2DCompilationPhase::Cancelled, "Texture build was cancelled during shutdown.");
			Scope.Close(ETaskScopeCloseMode::Drain);
			// RunningCount includes roots selected but not yet bound. Acknowledgement
			// precedes queue close, so transferred tickets never race owner teardown.
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
			if (Operations) Operations->Close();
		}

		FTextureCompilingManagerConfig Config;
		FTaskScope Scope;
		FTaskAttribution Attribution;
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::shared_ptr<FJob>> Jobs;
		std::deque<std::shared_ptr<FJob>> InteractiveQueue;
		std::deque<std::shared_ptr<FJob>> BackgroundQueue;
		std::unique_ptr<FOperations> Operations;
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
		Result.PendingCompletionCount = (QueueState->Operations ? QueueState->Operations->GetReadyCount() : 0);
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

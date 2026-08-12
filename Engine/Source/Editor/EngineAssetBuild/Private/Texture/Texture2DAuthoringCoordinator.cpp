#include "Texture/Texture2DAuthoringCoordinator.h"

#include "DObject/DObjectGlobals.h"
#include "Texture/TextureBuildOperations.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace Durin::AssetBuild
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

		auto EstimateBuildBytes(const FTexture2DQueuedBuildRequest& Request) -> uint64
		{
			uint64 PixelCount = SaturatingMultiply(Request.EstimatedWidth, Request.EstimatedHeight);
			uint64 WorkingBytes = SaturatingMultiply(PixelCount, 12);
			if (WorkingBytes == 0)
				WorkingBytes = std::max<uint64>(
					SaturatingMultiply(Request.SourceData.Pixels.size(), 3),
					64ull * 1024ull * 1024ull);
			return SaturatingAdd(Request.SourceData.Pixels.size(), WorkingBytes);
		}

		auto PlatformDataBytes(const FTexturePlatformData& PlatformData) -> uint64
		{
			uint64 Bytes = 0;
			for (const FTexture2DMipData& Mip : PlatformData.Mips)
				Bytes = SaturatingAdd(Bytes, Mip.Pixels.size());
			return Bytes;
		}

	}

	struct FTexture2DBuildCoordinator::FState final
		: public std::enable_shared_from_this<FTexture2DBuildCoordinator::FState>
	{
		struct FJob
		{
			FTexture2DQueuedBuildRequest Request;
			FTexture2DBuildCompletion Completion;
			FTexture2DBuildDiagnostic Diagnostic;
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
			FTexture2DQueuedBuildResult Result;
		};

		explicit FState(FTexture2DBuildCoordinatorConfig InConfig)
			: Config(InConfig)
		{
			Config.MaxWorkers = std::max(Config.MaxWorkers, 1u);
			Config.InteractiveBurstLimit = std::max(Config.InteractiveBurstLimit, 1u);
			Config.InFlightByteBudget = std::max<uint64>(Config.InFlightByteBudget, 1);
		}

		auto Submit(FTexture2DQueuedBuildRequest Request, FTexture2DBuildCompletion Completion) -> uint64
		{
			if (!Completion || Request.AssetIdentity.empty() || !Request.SourceData.IsValid()
				|| (Request.SourceHash.HashLow == 0 && Request.SourceHash.HashHigh == 0)) return 0;
			auto Job = std::make_shared<FJob>();
			Job->Request = std::move(Request);
			Job->Completion = std::move(Completion);
			Job->EstimatedBytes = EstimateBuildBytes(Job->Request);
			Job->EnqueueNanoseconds = NowNanoseconds();
			{
				std::lock_guard Lock(Mutex);
				if (!bAccepting) return 0;
				Job->Diagnostic.RequestId = NextRequestId++;
				Job->Diagnostic.Generation = Job->Request.Generation;
				Job->Diagnostic.AssetIdentity = Job->Request.AssetIdentity;
				Job->Diagnostic.Phase = ETexture2DBuildPhase::Queued;
				Job->Diagnostic.Metrics.EstimatedBytes = Job->EstimatedBytes;
				Jobs.emplace(Job->Diagnostic.RequestId, Job);
				if (Job->Request.Priority == ETexture2DBuildPriority::Interactive)
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
				while (bAccepting && RunningCount < Config.MaxWorkers)
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
						if (Job->Request.Priority == ETexture2DBuildPriority::Interactive)
							InteractiveQueue.push_front(std::move(Job));
						else BackgroundQueue.push_front(std::move(Job));
						break;
					}
					++RunningCount;
					InFlightEstimatedBytes = SaturatingAdd(InFlightEstimatedBytes, Job->EstimatedBytes);
					if (Job->Request.Priority == ETexture2DBuildPriority::Interactive)
						++ConsecutiveInteractive;
					else ConsecutiveInteractive = 0;
					Admitted.push_back(std::move(Job));
				}
			}
			for (const std::shared_ptr<FJob>& Job : Cancelled)
				CompleteWithoutWorker(Job, ETexture2DBuildPhase::Cancelled, "Texture build was cancelled before admission.");
			for (const std::shared_ptr<FJob>& Job : Admitted) Launch(Job);
		}

		auto Launch(const std::shared_ptr<FJob>& Job) -> void
		{
			const std::shared_ptr<FState> Self = shared_from_this();
			FTaskHandle Task = LaunchCancelableTask(
				"Texture2D.Build",
				[Self, Job](const FTaskCancellationToken& Token) {
					Self->RunWorker(Job, Token);
				});
			if (!Task.IsValid())
			{
				FTexture2DQueuedBuildResult Result = MakeFailureResult(
					*Job, ETexture2DBuildPhase::Failed,
					"Texture build task admission was rejected.");
				Result.FailurePhase = ETexture2DBuildPhase::Queued;
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			std::lock_guard JobLock(Job->Mutex);
			if (!Job->bWorkerCompleted.load(std::memory_order_acquire)) Job->Task = std::move(Task);
		}

		static auto MakeFailureResult(
			const FJob& Job,
			ETexture2DBuildPhase Phase,
			std::string Error) -> FTexture2DQueuedBuildResult
		{
			return {
				.RequestId = Job.Diagnostic.RequestId,
				.Generation = Job.Request.Generation,
				.AssetIdentity = Job.Request.AssetIdentity,
				.SourcePath = Job.Request.SourcePath,
				.Settings = Job.Request.Settings,
				.Error = std::move(Error),
				.Metrics = {.EstimatedBytes = Job.EstimatedBytes},
				.Phase = Phase};
		}

		auto SetPhase(const std::shared_ptr<FJob>& Job, ETexture2DBuildPhase Phase) -> void
		{
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->Diagnostic.Phase = Phase;
			}
			NotifyPhaseHook(Job->Diagnostic.RequestId, Phase);
		}

		auto NotifyPhaseHook(uint64 RequestId, ETexture2DBuildPhase Phase) -> void
		{
			std::function<void(uint64, ETexture2DBuildPhase)> Hook;
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
			FTexture2DQueuedBuildResult Result = MakeFailureResult(
				*Job, ETexture2DBuildPhase::Failed, {});
			const uint64 WorkerStart = NowNanoseconds();
			{
				std::lock_guard JobLock(Job->Mutex);
				Job->WorkerStartNanoseconds = WorkerStart;
				Job->Diagnostic.QueuedNanoseconds = WorkerStart - Job->EnqueueNanoseconds;
			}
			const auto Cancel = [&] { return IsCancelled(Job, Token); };
			if (Cancel())
			{
				Result.Phase = ETexture2DBuildPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				CompleteAdmitted(Job, std::move(Result));
				return;
			}

			SetPhase(Job, ETexture2DBuildPhase::Preparing);
			if (Cancel())
			{
				Result.Phase = ETexture2DBuildPhase::Cancelled;
				Result.Error = "Texture build was cancelled.";
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			const uint64 PreparationStart = NowNanoseconds();
			auto SourceData = std::make_unique<FTextureSourceData>(
				std::move(Job->Request.SourceData));
			Result.Metrics.PreparationNanoseconds = NowNanoseconds() - PreparationStart;
			Result.Metrics.DecodedBytes = SourceData->Pixels.size();
			Result.SourceHash = Job->Request.SourceHash;

			SetPhase(Job, ETexture2DBuildPhase::Building);
			const FTexture2DBuildSettingsSnapshot& Settings = Job->Request.Settings;
			FTexture2DRecipeMetrics RecipeMetrics;
			bool bEnteredPersisting = false;
			const FTexture2DBuildExecutionControl Control{
				.ShouldCancel = Cancel,
				.OnPersisting = [&] {
					bEnteredPersisting = true;
					SetPhase(Job, ETexture2DBuildPhase::Persisting);
				},
				.Metrics = &RecipeMetrics};
			FTexture2DBuildProduct Product;
			if (!BuildTexture2D({
				.SourceData = std::move(*SourceData),
				.SourceContentHashLow = Result.SourceHash.HashLow,
				.SourceContentHashHigh = Result.SourceHash.HashHigh,
				.Settings = {
					.Usage = Settings.Usage,
					.CompressionQuality = Settings.CompressionQuality,
					.AlphaMipMode = Settings.AlphaMipMode,
					.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
					.MaxResolution = Settings.MaxResolution,
					.bSRGB = Settings.bSRGB},
				.bPersistDerivedData = Job->Request.bPersistDerivedData},
				Product, Result.Error, &Control))
			{
				Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
				Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
				Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
				Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
				Result.Phase = Cancel() ? ETexture2DBuildPhase::Cancelled : ETexture2DBuildPhase::Failed;
				if (Result.Phase == ETexture2DBuildPhase::Failed)
					Result.FailurePhase = bEnteredPersisting
						? ETexture2DBuildPhase::Persisting : ETexture2DBuildPhase::Building;
				CompleteAdmitted(Job, std::move(Result));
				return;
			}
			Result.Metrics.MipGenerationNanoseconds = RecipeMetrics.MipGenerationNanoseconds;
			Result.Metrics.CompressionNanoseconds = RecipeMetrics.CompressionNanoseconds;
			Result.Metrics.PersistenceNanoseconds = RecipeMetrics.PersistenceNanoseconds;
			Result.Metrics.PeakIntermediateBytes = RecipeMetrics.PeakIntermediateBytes;
			Result.Metrics.ResultBytes = PlatformDataBytes(Product.PlatformData);
			Result.DerivedDataKey = std::move(Product.DerivedDataKey);
			Result.SourceData = std::make_unique<FTextureSourceData>(std::move(Product.SourceData));
			Result.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData));
			Result.Error.clear();
			Result.Phase = Cancel() ? ETexture2DBuildPhase::Cancelled : ETexture2DBuildPhase::UploadPending;
			Result.Metrics.WorkerNanoseconds = NowNanoseconds() - WorkerStart;
			CompleteAdmitted(Job, std::move(Result));
		}

		auto CompleteWithoutWorker(
			const std::shared_ptr<FJob>& Job,
			ETexture2DBuildPhase Phase,
			std::string Error) -> void
		{
			FTexture2DQueuedBuildResult Result = MakeFailureResult(*Job, Phase, std::move(Error));
			std::vector<uint8>().swap(Job->Request.SourceData.Pixels);
			PushCompletion(Job, std::move(Result));
		}

		auto CompleteAdmitted(
			const std::shared_ptr<FJob>& Job,
			FTexture2DQueuedBuildResult Result) -> void
		{
			Job->bWorkerCompleted.store(true, std::memory_order_release);
			{
				std::lock_guard Lock(Mutex);
				check(RunningCount > 0);
				--RunningCount;
				InFlightEstimatedBytes -= std::min(InFlightEstimatedBytes, Job->EstimatedBytes);
			}
			std::vector<uint8>().swap(Job->Request.SourceData.Pixels);
			PushCompletion(Job, std::move(Result));
			Admit();
		}

		auto PushCompletion(
			const std::shared_ptr<FJob>& Job,
			FTexture2DQueuedBuildResult Result) -> void
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
				Job->Diagnostic.QueuedNanoseconds = Job->WorkerStartNanoseconds != 0
					? Job->WorkerStartNanoseconds - Job->EnqueueNanoseconds
					: NowNanoseconds() - Job->EnqueueNanoseconds;
				Job->Diagnostic.WorkerNanoseconds = Result.Metrics.WorkerNanoseconds;
				Job->bCompletionQueued = true;
			}
			const ETexture2DBuildPhase FinalPhase = Result.Phase;
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

		auto CancelAsset(std::string_view AssetIdentity) -> uint32
		{
			std::vector<std::shared_ptr<FJob>> Matching;
			{
				std::lock_guard Lock(Mutex);
				for (const auto& [RequestId, Job] : Jobs)
				{
					(void)RequestId;
					if (Job->Request.AssetIdentity == AssetIdentity) Matching.push_back(Job);
				}
			}
			uint32 Count = 0;
			for (const std::shared_ptr<FJob>& Job : Matching)
			{
				std::lock_guard JobLock(Job->Mutex);
				if (!Job->bCompletionQueued
					&& !Job->bCancellationRequested.exchange(true, std::memory_order_acq_rel)) ++Count;
			}
			return Count;
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

		auto Shutdown() -> void
		{
			std::vector<FTaskHandle> Tasks;
			std::vector<std::shared_ptr<FJob>> Queued;
			{
				std::lock_guard Lock(Mutex);
				if (bShutdown) return;
				bAccepting = false;
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
				CompleteWithoutWorker(Job, ETexture2DBuildPhase::Cancelled, "Texture build was cancelled during shutdown.");
			if (!Tasks.empty()) WaitAll(Tasks);
			Pump(std::numeric_limits<uint32>::max());
		}

		FTexture2DBuildCoordinatorConfig Config;
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
		bool bAccepting = true;
		bool bShutdown = false;
		std::function<void(uint64, ETexture2DBuildPhase)> PhaseHookForTests;
	};

	FTexture2DBuildCoordinator::FTexture2DBuildCoordinator(
		const FTexture2DBuildCoordinatorConfig& Config)
		: State(std::make_shared<FState>(Config))
	{}

	FTexture2DBuildCoordinator::~FTexture2DBuildCoordinator()
	{
		Shutdown();
	}

	auto FTexture2DBuildCoordinator::Submit(
		FTexture2DQueuedBuildRequest Request,
		FTexture2DBuildCompletion Completion) -> uint64
	{
		return State ? State->Submit(std::move(Request), std::move(Completion)) : 0;
	}

	auto FTexture2DBuildCoordinator::Cancel(uint64 RequestId) -> bool
	{
		return State && State->Cancel(RequestId);
	}

	auto FTexture2DBuildCoordinator::CancelAsset(std::string_view AssetIdentity) -> uint32
	{
		return State ? State->CancelAsset(AssetIdentity) : 0;
	}

	auto FTexture2DBuildCoordinator::GetDiagnostic(uint64 RequestId) const -> FTexture2DBuildDiagnostic
	{
		if (!State) return {};
		std::shared_ptr<FState::FJob> Job;
		{
			std::lock_guard Lock(State->Mutex);
			const auto Iterator = State->Jobs.find(RequestId);
			if (Iterator == State->Jobs.end()) return {};
			Job = Iterator->second;
		}
		std::lock_guard JobLock(Job->Mutex);
		FTexture2DBuildDiagnostic Result = Job->Diagnostic;
		if (Result.Phase == ETexture2DBuildPhase::Queued)
			Result.QueuedNanoseconds = NowNanoseconds() - Job->EnqueueNanoseconds;
		else if (Job->WorkerStartNanoseconds != 0
			&& Result.Phase != ETexture2DBuildPhase::UploadPending
			&& Result.Phase != ETexture2DBuildPhase::Ready
			&& Result.Phase != ETexture2DBuildPhase::Failed
			&& Result.Phase != ETexture2DBuildPhase::Cancelled)
			Result.WorkerNanoseconds = NowNanoseconds() - Job->WorkerStartNanoseconds;
		return Result;
	}

	auto FTexture2DBuildCoordinator::GetQueuedCount() const -> uint32
	{
		if (!State) return 0;
		std::lock_guard Lock(State->Mutex);
		return static_cast<uint32>(State->InteractiveQueue.size() + State->BackgroundQueue.size());
	}

	auto FTexture2DBuildCoordinator::GetRunningCount() const -> uint32
	{
		if (!State) return 0;
		std::lock_guard Lock(State->Mutex);
		return State->RunningCount;
	}

	auto FTexture2DBuildCoordinator::GetInFlightEstimatedBytes() const -> uint64
	{
		if (!State) return 0;
		std::lock_guard Lock(State->Mutex);
		return State->InFlightEstimatedBytes;
	}

	auto FTexture2DBuildCoordinator::SetPhaseHookForTests(
		std::function<void(uint64, ETexture2DBuildPhase)> Hook) -> void
	{
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		State->PhaseHookForTests = std::move(Hook);
	}

	auto FTexture2DBuildCoordinator::PumpCompletions(uint32 MaximumCount) -> uint32
	{
		return State ? State->Pump(MaximumCount) : 0;
	}

	auto FTexture2DBuildCoordinator::WaitForRequest(
		uint64 RequestId, double TimeoutSeconds) -> bool
	{
		return State && State->WaitForRequest(RequestId, TimeoutSeconds);
	}

	auto FTexture2DBuildCoordinator::Shutdown() -> void
	{
		if (State) State->Shutdown();
	}

	namespace
	{
		std::mutex GTexture2DBuildCoordinatorMutex;
		std::unique_ptr<FTexture2DBuildCoordinator> GTexture2DBuildCoordinator;
	}

	auto InitializeTexture2DBuildCoordinator(
		const FTexture2DBuildCoordinatorConfig& Config) -> bool
	{
		std::lock_guard Lock(GTexture2DBuildCoordinatorMutex);
		if (GTexture2DBuildCoordinator) return true;
		GTexture2DBuildCoordinator = std::make_unique<FTexture2DBuildCoordinator>(Config);
		return true;
	}

	auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*
	{
		std::lock_guard Lock(GTexture2DBuildCoordinatorMutex);
		return GTexture2DBuildCoordinator.get();
	}

	auto PumpTexture2DBuildCompletions(uint32 MaximumCount) -> uint32
	{
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		return Coordinator ? Coordinator->PumpCompletions(MaximumCount) : 0;
	}

	auto ShutdownTexture2DBuildCoordinator() -> void
	{
		std::unique_ptr<FTexture2DBuildCoordinator> Coordinator;
		{
			std::lock_guard Lock(GTexture2DBuildCoordinatorMutex);
			Coordinator = std::move(GTexture2DBuildCoordinator);
		}
		if (Coordinator) Coordinator->Shutdown();
	}
}

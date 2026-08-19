#include "Threading/Task.h"

#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	class FGameThreadDeferredWorkQueue;

	namespace Private
	{
		struct FTaskAttributionAccess
		{
			static constexpr auto Make(uint16 OwnerId, uint16 CategoryId) -> FTaskAttribution
			{
				return FTaskAttribution(OwnerId, CategoryId);
			}

			static constexpr auto GetOwnerId(FTaskAttribution Attribution) -> uint16 { return Attribution.OwnerId; }
			static constexpr auto GetCategoryId(FTaskAttribution Attribution) -> uint16 { return Attribution.CategoryId; }
			static constexpr auto IsDefault(FTaskAttribution Attribution) -> bool
			{
				return Attribution.OwnerId == 0 && Attribution.CategoryId == 0;
			}
		};

		struct FTaskScopeAccess
		{
			static auto GetState(const FTaskScopeToken& Token) -> const std::shared_ptr<FTaskScopeState>& { return Token.State; }
		};
	}

	namespace
	{
		constexpr uint16 UnattributedAttributionId = 0;
		constexpr uint16 OverflowAttributionId = 1;
		constexpr uint16 MaxTaskAttributionOwners = 256;
		constexpr uint16 MaxTaskAttributionPairs = 1'024;
		constexpr size_t MaxTaskAttributionLabelBytes = 63;

		struct FTaskAttributionLabel
		{
			std::array<char, MaxTaskAttributionLabelBytes + 1> Bytes{};
			uint8 Length = 0;

			auto View() const -> std::string_view { return {Bytes.data(), Length}; }
		};

		struct FTaskAttributionPair
		{
			uint16 OwnerId = 0;
			FTaskAttributionLabel Category;
		};

		auto IsValidTaskAttributionLabel(std::string_view Label) -> bool
		{
			if (Label.empty() || Label.size() > MaxTaskAttributionLabelBytes || Label.find('\0') != std::string_view::npos)
			{
				return false;
			}
			for (size_t Index = 0; Index < Label.size();)
			{
				const uint8 Lead = static_cast<uint8>(Label[Index]);
				size_t ContinuationCount = 0;
				uint32 CodePoint = 0;
				uint32 MinimumCodePoint = 0;
				if (Lead <= 0x7f)
				{
					++Index;
					continue;
				}
				if ((Lead & 0xe0) == 0xc0) { ContinuationCount = 1; CodePoint = Lead & 0x1f; MinimumCodePoint = 0x80; }
				else if ((Lead & 0xf0) == 0xe0) { ContinuationCount = 2; CodePoint = Lead & 0x0f; MinimumCodePoint = 0x800; }
				else if ((Lead & 0xf8) == 0xf0) { ContinuationCount = 3; CodePoint = Lead & 0x07; MinimumCodePoint = 0x10000; }
				else return false;
				if (Index + ContinuationCount >= Label.size()) return false;
				for (size_t Offset = 1; Offset <= ContinuationCount; ++Offset)
				{
					const uint8 Continuation = static_cast<uint8>(Label[Index + Offset]);
					if ((Continuation & 0xc0) != 0x80) return false;
					CodePoint = (CodePoint << 6) | (Continuation & 0x3f);
				}
				if (CodePoint < MinimumCodePoint || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
				Index += ContinuationCount + 1;
			}
			return true;
		}

		auto CopyTaskAttributionLabel(std::string_view Label) -> FTaskAttributionLabel
		{
			FTaskAttributionLabel Result;
			Result.Length = static_cast<uint8>(Label.size());
			std::ranges::copy(Label, Result.Bytes.begin());
			return Result;
		}

		class FTaskAttributionRegistry
		{
		public:
			FTaskAttributionRegistry()
			{
				InitializeReservedAttributions();
#if DURIN_WITH_TRACY
				Profiling::RegisterTaskProfilerAttribution(UnattributedAttributionId, UnattributedAttributionId, "Unattributed", "Unattributed");
				Profiling::RegisterTaskProfilerAttribution(OverflowAttributionId, OverflowAttributionId, "Overflow", "Overflow");
#endif
			}

			auto Register(std::string_view Owner, std::string_view Category) -> FTaskAttribution
			{
				if (!IsValidTaskAttributionLabel(Owner) || !IsValidTaskAttributionLabel(Category))
				{
					OverflowCount.fetch_add(1, std::memory_order::acq_rel);
					return Private::FTaskAttributionAccess::Make(OverflowAttributionId, OverflowAttributionId);
				}

				std::lock_guard Lock(Mutex);
				for (uint16 CategoryId = 2; CategoryId < PairCount; ++CategoryId)
				{
					const FTaskAttributionPair& Pair = Pairs[CategoryId];
					if (Owners[Pair.OwnerId].View() == Owner && Pair.Category.View() == Category)
					{
						return Private::FTaskAttributionAccess::Make(Pair.OwnerId, CategoryId);
					}
				}

				uint16 OwnerId = MaxTaskAttributionOwners;
				for (uint16 CandidateId = 2; CandidateId < OwnerCount; ++CandidateId)
				{
					if (Owners[CandidateId].View() == Owner)
					{
						OwnerId = CandidateId;
						break;
					}
				}
				if (PairCount == MaxTaskAttributionPairs || (OwnerId == MaxTaskAttributionOwners && OwnerCount == MaxTaskAttributionOwners))
				{
					OverflowCount.fetch_add(1, std::memory_order::acq_rel);
					return Private::FTaskAttributionAccess::Make(OverflowAttributionId, OverflowAttributionId);
				}
				if (OwnerId == MaxTaskAttributionOwners)
				{
					OwnerId = OwnerCount++;
					Owners[OwnerId] = CopyTaskAttributionLabel(Owner);
				}
				const uint16 CategoryId = PairCount++;
				Pairs[CategoryId] = {OwnerId, CopyTaskAttributionLabel(Category)};
#if DURIN_WITH_TRACY
				Profiling::RegisterTaskProfilerAttribution(OwnerId, CategoryId, Owner, Category);
#endif
				return Private::FTaskAttributionAccess::Make(OwnerId, CategoryId);
			}

			auto Resolve(FTaskAttribution Attribution) const -> std::pair<std::string, std::string>
			{
				const uint16 OwnerId = Private::FTaskAttributionAccess::GetOwnerId(Attribution);
				const uint16 CategoryId = Private::FTaskAttributionAccess::GetCategoryId(Attribution);
				std::lock_guard Lock(Mutex);
				if (OwnerId >= OwnerCount || CategoryId >= PairCount || Pairs[CategoryId].OwnerId != OwnerId)
				{
					return {"Overflow", "Overflow"};
				}
				return {std::string(Owners[OwnerId].View()), std::string(Pairs[CategoryId].Category.View())};
			}

			auto Snapshot() const -> std::vector<FTaskOwnerCategoryDiagnostics>
			{
				std::lock_guard Lock(Mutex);
				std::vector<FTaskOwnerCategoryDiagnostics> Result;
				Result.reserve(PairCount);
				for (uint16 CategoryId = 0; CategoryId < PairCount; ++CategoryId)
				{
					const FTaskAttributionPair& Pair = Pairs[CategoryId];
					FTaskOwnerCategoryDiagnostics& Entry = Result.emplace_back();
					Entry.OwnerId = Pair.OwnerId;
					Entry.CategoryId = CategoryId;
					Entry.Owner = Owners[Pair.OwnerId].View();
					Entry.Category = Pair.Category.View();
				}
				return Result;
			}

			auto GetOverflowCount() const -> uint64 { return OverflowCount.load(std::memory_order::acquire); }

			void ResetForTests()
			{
				std::lock_guard Lock(Mutex);
				Owners = {};
				Pairs = {};
				OwnerCount = 2;
				PairCount = 2;
				OverflowCount.store(0, std::memory_order::release);
				InitializeReservedAttributions();
			}

		private:
			void InitializeReservedAttributions()
			{
				Owners[UnattributedAttributionId] = CopyTaskAttributionLabel("Unattributed");
				Owners[OverflowAttributionId] = CopyTaskAttributionLabel("Overflow");
				Pairs[UnattributedAttributionId] = {UnattributedAttributionId, CopyTaskAttributionLabel("Unattributed")};
				Pairs[OverflowAttributionId] = {OverflowAttributionId, CopyTaskAttributionLabel("Overflow")};
			}

			mutable std::mutex Mutex;
			std::array<FTaskAttributionLabel, MaxTaskAttributionOwners> Owners{};
			std::array<FTaskAttributionPair, MaxTaskAttributionPairs> Pairs{};
			uint16 OwnerCount = 2;
			uint16 PairCount = 2;
			std::atomic<uint64> OverflowCount = 0;
		};

		FTaskAttributionRegistry GTaskAttributionRegistry;

		enum class ETaskAggregateCounter : uint8
		{
			Accepted,
			Succeeded,
			Failed,
			Canceled,
			Rejected,
			DependencyFailed,
			DependencyCanceled,
			CancellationRequested,
			DispatchRejected,
			CapacityExhausted,
			Superseded,
			StaleGeneration,
			CallbackFailure,
			ShutdownCanceled,
			ParallelForOperation,
			Count,
		};

		enum class ETaskAggregateGauge : uint8
		{
			Waiting,
			Queued,
			Running,
			Nonterminal,
			CallableBytes,
			PayloadBytes,
			ResultBytes,
			RetainedUniqueResultBytes,
			Count,
		};

		enum class ETaskAggregateHistogram : uint8
		{
			QueueResidency,
			Execution,
			CallableBytes,
			PayloadBytes,
			ResultBytes,
			Count,
		};

		constexpr auto AggregateIndex(auto Value) -> size_t
		{
			return static_cast<size_t>(Value);
		}

		auto TaskHistogramBucket(uint64 Value) -> size_t
		{
			if (Value == 0) return 0;
			return std::min<size_t>(31, 64 - static_cast<size_t>(std::countl_zero(Value)));
		}

		struct FTaskOwnerCategoryAggregate
		{
			auto Increment(ETaskAggregateCounter Counter) -> void
			{
				Counters[AggregateIndex(Counter)].fetch_add(1, std::memory_order::acq_rel);
			}

			auto Add(ETaskAggregateGauge Gauge, uint64 Value) -> void
			{
				if (Value == 0) return;
				const size_t Index = AggregateIndex(Gauge);
				const uint64 CurrentValue = Current[Index].fetch_add(Value, std::memory_order::acq_rel) + Value;
				uint64 PeakValue = Peak[Index].load(std::memory_order::acquire);
				while (PeakValue < CurrentValue && !Peak[Index].compare_exchange_weak(PeakValue, CurrentValue, std::memory_order::acq_rel)) {}
			}

			auto Subtract(ETaskAggregateGauge Gauge, uint64 Value) -> void
			{
				if (Value == 0) return;
				const uint64 PreviousValue = Current[AggregateIndex(Gauge)].fetch_sub(Value, std::memory_order::acq_rel);
				check(PreviousValue >= Value);
			}

			auto Record(ETaskAggregateHistogram Histogram, uint64 Value) -> void
			{
				Histograms[AggregateIndex(Histogram)][TaskHistogramBucket(Value)].fetch_add(1, std::memory_order::acq_rel);
			}

			auto Snapshot(FTaskOwnerCategoryDiagnostics& Out) const -> void
			{
				auto Counter = [this](ETaskAggregateCounter Value) { return Counters[AggregateIndex(Value)].load(std::memory_order::acquire); };
				auto Gauge = [this](ETaskAggregateGauge Value) { return Current[AggregateIndex(Value)].load(std::memory_order::acquire); };
				auto GaugePeak = [this](ETaskAggregateGauge Value) { return Peak[AggregateIndex(Value)].load(std::memory_order::acquire); };
				Out.AcceptedCount = Counter(ETaskAggregateCounter::Accepted);
				Out.SucceededCount = Counter(ETaskAggregateCounter::Succeeded);
				Out.FailedCount = Counter(ETaskAggregateCounter::Failed);
				Out.CanceledCount = Counter(ETaskAggregateCounter::Canceled);
				Out.RejectedCount = Counter(ETaskAggregateCounter::Rejected);
				Out.DependencyFailedCount = Counter(ETaskAggregateCounter::DependencyFailed);
				Out.DependencyCanceledCount = Counter(ETaskAggregateCounter::DependencyCanceled);
				Out.CancellationRequestedCount = Counter(ETaskAggregateCounter::CancellationRequested);
				Out.DispatchRejectedCount = Counter(ETaskAggregateCounter::DispatchRejected);
				Out.CapacityExhaustedCount = Counter(ETaskAggregateCounter::CapacityExhausted);
				Out.SupersededCount = Counter(ETaskAggregateCounter::Superseded);
				Out.StaleGenerationCount = Counter(ETaskAggregateCounter::StaleGeneration);
				Out.CallbackFailureCount = Counter(ETaskAggregateCounter::CallbackFailure);
				Out.ShutdownCanceledCount = Counter(ETaskAggregateCounter::ShutdownCanceled);
				Out.ParallelForOperationCount = Counter(ETaskAggregateCounter::ParallelForOperation);
				Out.CurrentWaitingCount = Gauge(ETaskAggregateGauge::Waiting);
				Out.CurrentQueuedCount = Gauge(ETaskAggregateGauge::Queued);
				Out.CurrentRunningCount = Gauge(ETaskAggregateGauge::Running);
				Out.CurrentNonterminalCount = Gauge(ETaskAggregateGauge::Nonterminal);
				Out.CurrentCallableBytes = Gauge(ETaskAggregateGauge::CallableBytes);
				Out.PeakCallableBytes = GaugePeak(ETaskAggregateGauge::CallableBytes);
				Out.CurrentPayloadBytes = Gauge(ETaskAggregateGauge::PayloadBytes);
				Out.PeakPayloadBytes = GaugePeak(ETaskAggregateGauge::PayloadBytes);
				Out.CurrentResultBytes = Gauge(ETaskAggregateGauge::ResultBytes);
				Out.PeakResultBytes = GaugePeak(ETaskAggregateGauge::ResultBytes);
				Out.CurrentRetainedUniqueResultBytes = Gauge(ETaskAggregateGauge::RetainedUniqueResultBytes);
				Out.PeakRetainedUniqueResultBytes = GaugePeak(ETaskAggregateGauge::RetainedUniqueResultBytes);
				for (size_t Bucket = 0; Bucket < 32; ++Bucket)
				{
					Out.QueueResidencyHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::QueueResidency)][Bucket].load(std::memory_order::acquire);
					Out.ExecutionHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::Execution)][Bucket].load(std::memory_order::acquire);
					Out.CallableBytesHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::CallableBytes)][Bucket].load(std::memory_order::acquire);
					Out.PayloadBytesHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::PayloadBytes)][Bucket].load(std::memory_order::acquire);
					Out.ResultBytesHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::ResultBytes)][Bucket].load(std::memory_order::acquire);
				}
			}

			std::array<std::atomic<uint64>, AggregateIndex(ETaskAggregateCounter::Count)> Counters{};
			std::array<std::atomic<uint64>, AggregateIndex(ETaskAggregateGauge::Count)> Current{};
			std::array<std::atomic<uint64>, AggregateIndex(ETaskAggregateGauge::Count)> Peak{};
			std::array<std::array<std::atomic<uint64>, 32>, AggregateIndex(ETaskAggregateHistogram::Count)> Histograms{};
		};

		struct FTaskAccountingSnapshot
		{
			FTaskAttribution Attribution;
			ETaskState State = ETaskState::Invalid;
			ETaskState StateBeforeTerminal = ETaskState::Invalid;
			ETaskTerminalReason TerminalReason = ETaskTerminalReason::None;
			uint64 CallableBytes = 0;
			uint64 PayloadBytes = 0;
			uint64 ResultBytes = 0;
			uint64 RetainedResultBytes = 0;
			uint64 QueueResidencyNanoseconds = 0;
			uint64 ExecutionNanoseconds = 0;
		};

		struct FTaskSchedulerLifetimeAccounting
		{
			std::atomic<uint64> RetainedTerminalTaskCount = 0;
			std::atomic<uint64> RetainedTerminalResultCount = 0;
		};

		enum class ETaskSchedulerLifetime : uint8
		{
			Stopped,
			Running,
			ShuttingDown,
		};

		std::atomic<uint64> GNextTaskId = 1;
		std::atomic<uint64> GNextTaskScopeId = 1;
		std::mutex GTaskSchedulerMutex;
		std::condition_variable GTaskSchedulerCV;
		std::shared_ptr<FTaskScheduler> GTaskScheduler;
		ETaskSchedulerLifetime GTaskSchedulerLifetime = ETaskSchedulerLifetime::Stopped;
		uint64 GCompletedTaskSchedulerShutdowns = 0;
		std::shared_ptr<const FTaskSchedulerDiagnostics> GLastTaskSchedulerDiagnostics;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> GLastTaskSchedulerLifetimeAccounting;
		std::mutex GTaskTerminalPublicationTestHookMutex;
		std::function<void(uint64)> GTaskTerminalPublicationTestHook;
		std::atomic<bool> GTaskTerminalPublicationTestHookEnabled = false;
		std::mutex GTaskSchedulerSnapshotTestHookMutex;
		std::function<void()> GTaskSchedulerSnapshotTestHook;
		std::atomic<bool> GTaskSchedulerSnapshotTestHookEnabled = false;
		std::mutex GGameThreadDeferredQueueMutex;
		std::shared_ptr<FGameThreadDeferredWorkQueue> GGameThreadDeferredQueue;
		FGameThreadDeferredWorkQueueDiagnostics GLastGameThreadDeferredQueueDiagnostics;
		std::atomic<uint64> GNextGameThreadDeferredAdapterGeneration = 1;

		thread_local FTaskStateData* GCurrentTaskState = nullptr;
		thread_local FTaskScheduler* GCurrentTaskScheduler = nullptr;
		thread_local uint32 GParallelForDepth = 0;
		thread_local bool GIsPumpingGameThreadDeferred = false;

		constexpr double WorkerWaitSliceSeconds = 0.001;
		constexpr double LongWaitThresholdSeconds = 0.1;

		auto MonotonicNanoseconds() -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()
			).count());
		}

		auto IsTerminalState(ETaskState State) -> bool
		{
			return State == ETaskState::Succeeded || State == ETaskState::Failed || State == ETaskState::Canceled;
		}

		auto InvokeTaskTerminalPublicationTestHook(uint64 TaskId) -> void
		{
			if (!GTaskTerminalPublicationTestHookEnabled.load(std::memory_order::acquire))
			{
				return;
			}
			std::function<void(uint64)> Hook;
			{
				std::lock_guard Lock(GTaskTerminalPublicationTestHookMutex);
				Hook = GTaskTerminalPublicationTestHook;
			}
			if (Hook)
			{
				Hook(TaskId);
			}
		}
	} // namespace

	auto RegisterTaskAttribution(std::string_view Owner, std::string_view Category) -> FTaskAttribution
	{
		return GTaskAttributionRegistry.Register(Owner, Category);
	}

	void Private::ResetTaskAttributionRegistryForTests()
	{
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			check(GTaskSchedulerLifetime == ETaskSchedulerLifetime::Stopped);
		}
		GTaskAttributionRegistry.ResetForTests();
	}

	struct FTaskScopeSchedulerAccounting
	{
		std::atomic<uint64> RejectedTaskCount = 0;
		std::atomic<uint64> AbandonedOpenScopeCount = 0;
	};

	class FTaskCancellationState
	{
	public:
		auto IsCancellationRequested() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bCancellationRequested;
		}

		auto RegisterTask(const std::shared_ptr<FTaskStateData>& Task) -> void;
		auto UnregisterTask(uint64 TaskId) -> void;
		auto RequestCancellation() -> void;

	private:
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::weak_ptr<FTaskStateData>> Tasks;
		bool bCancellationRequested = false;
	};

	class FTaskScopeState final
	{
	public:
		explicit FTaskScopeState(
			std::weak_ptr<FTaskScheduler> InScheduler,
			std::shared_ptr<FTaskScopeSchedulerAccounting> InSchedulerAccounting)
			: Scheduler(std::move(InScheduler))
			, SchedulerAccounting(std::move(InSchedulerAccounting))
			, ScopeId(GNextTaskScopeId.fetch_add(1, std::memory_order::acq_rel))
		{
		}

		auto TryAdmit(const FTaskScheduler* ExpectedScheduler) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskScopeState::Open || Scheduler.lock().get() != ExpectedScheduler)
			{
				++RejectedCount;
				return false;
			}
			++AcceptedCount;
			++CurrentActiveCount;
			PeakActiveCount = std::max(PeakActiveCount, CurrentActiveCount);
			return true;
		}

		auto RollbackAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			require(AcceptedCount > 0 && CurrentActiveCount > 0);
			--AcceptedCount;
			--CurrentActiveCount;
			if (CurrentActiveCount == 0)
			{
				if (State == ETaskScopeState::ClosingDrain) State = ETaskScopeState::QuiescentDrain;
				else if (State == ETaskScopeState::ClosingCancel) State = ETaskScopeState::QuiescentCancel;
				CV.notify_all();
			}
		}

		auto BindTask(const std::shared_ptr<FTaskStateData>& Task) -> bool;
		auto ReleaseTask(uint64 TaskId, ETaskState TerminalState) -> void;
		auto Close(ETaskScopeCloseMode Mode) -> ETaskScopeCloseResult;
		auto WaitFor(double TimeoutSeconds, bool bUnbounded) -> ETaskScopeWaitResult;
		auto GetDiagnostics() const -> FTaskScopeDiagnostics;
		auto AbandonOpen() -> void;
		auto GetRegistryState(bool& bOutOpen, bool& bOutNonquiescent) const -> void
		{
			std::lock_guard Lock(Mutex);
			bOutOpen = State == ETaskScopeState::Open;
			bOutNonquiescent = State != ETaskScopeState::QuiescentDrain && State != ETaskScopeState::QuiescentCancel;
		}

		auto RecordRejected() -> void
		{
			std::lock_guard Lock(Mutex);
			++RejectedCount;
		}

		auto GetScopeId() const -> uint64 { return ScopeId; }
		auto PinScheduler() const -> std::shared_ptr<FTaskScheduler> { return Scheduler.lock(); }
		auto ChargeRetainedResult() -> void
		{
			std::lock_guard Lock(Mutex);
			++CurrentRetainedResultCount;
			PeakRetainedResultCount = std::max(PeakRetainedResultCount, CurrentRetainedResultCount);
		}
		auto ReleaseRetainedResult() -> void
		{
			std::lock_guard Lock(Mutex);
			require(CurrentRetainedResultCount > 0);
			--CurrentRetainedResultCount;
			CV.notify_all();
		}

	private:
		mutable std::mutex Mutex;
		std::condition_variable CV;
		std::weak_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<FTaskScopeSchedulerAccounting> SchedulerAccounting;
		std::unordered_map<uint64, std::weak_ptr<FTaskStateData>> ActiveTasks;
		ETaskScopeState State = ETaskScopeState::Open;
		uint64 ScopeId = 0;
		uint64 AcceptedCount = 0;
		uint64 RejectedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CanceledCount = 0;
		uint64 CurrentActiveCount = 0;
		uint64 PeakActiveCount = 0;
		uint64 CurrentRetainedResultCount = 0;
		uint64 PeakRetainedResultCount = 0;
	};

	class FTaskGenerationState
	{
	public:
		std::atomic<uint64> Generation = 1;
	};

	// Owns one scheduler task node independently of public handles and pool work items.
	class FTaskStateData final : public std::enable_shared_from_this<FTaskStateData>
	{
	public:
		FTaskStateData(
			const char* InDebugName,
			std::weak_ptr<FTaskScheduler> InScheduler,
			std::shared_ptr<FTaskSchedulerLifetimeAccounting> InLifetimeAccounting,
			std::unique_ptr<Private::FMoveOnlyTaskFunction> InFunction,
			std::function<void(ETaskState)>&& InCompletionFunction,
			const FTaskCancellationToken& InCancellationToken,
			const std::vector<std::shared_ptr<FTaskStateData>>& InPrerequisites,
			uint64 InParentTaskId,
			ETaskDependencyKind InDependencyKind,
			bool bInAggregatePrerequisites,
			ETaskTarget InTarget,
			ETaskPriority InPriority,
			uint64 InEstimatedPayloadBytes,
			uint64 InEstimatedResultBytes,
			FTaskAttribution InAttribution,
			std::shared_ptr<FTaskScopeState> InScope,
			FTaskGenerationToken InGenerationToken,
			std::optional<FTaskCoalescingKey> InCoalescingKey
		)
			: TaskId(GNextTaskId.fetch_add(1, std::memory_order::acq_rel))
			, ParentTaskId(InParentTaskId)
			, DebugName(InDebugName ? InDebugName : "Task")
			, Scheduler(std::move(InScheduler))
			, LifetimeAccounting(std::move(InLifetimeAccounting))
			, SharedCancellationState(InCancellationToken.SharedState)
			, PendingFunction(std::move(InFunction))
			, CompletionFunction(std::move(InCompletionFunction))
			, bHasResultStorage(static_cast<bool>(CompletionFunction))
			, RemainingPrerequisites(static_cast<uint32>(InPrerequisites.size()))
			, State(InPrerequisites.empty() ? ETaskState::Queued : ETaskState::Waiting)
			, DependencyKind(InDependencyKind)
			, bAggregatePrerequisites(bInAggregatePrerequisites)
			, Target(InTarget)
			, Priority(InPriority)
			, EstimatedPayloadBytes(InEstimatedPayloadBytes)
			, EstimatedResultBytes(InEstimatedResultBytes)
			, Attribution(InAttribution)
			, Scope(std::move(InScope))
			, CallableStorageBytes(PendingFunction ? PendingFunction->GetStorageBytes() : 0)
			, GenerationToken(std::move(InGenerationToken))
			, CoalescingKey(std::move(InCoalescingKey))
			, EnqueueTimeNanoseconds(MonotonicNanoseconds())
		{
			Prerequisites.reserve(InPrerequisites.size());
			PrerequisiteTaskIds.reserve(InPrerequisites.size());
			for (const std::shared_ptr<FTaskStateData>& Prerequisite : InPrerequisites)
			{
				Prerequisites.emplace_back(Prerequisite);
				PrerequisiteTaskIds.emplace_back(Prerequisite->GetTaskId());
				PrerequisiteDependencyKinds.emplace_back(InDependencyKind);
			}
		}

		~FTaskStateData()
		{
			if (!bTerminalLifetimeCharged)
			{
				return;
			}
			const uint64 PreviousTaskCount = LifetimeAccounting->RetainedTerminalTaskCount.fetch_sub(1, std::memory_order::acq_rel);
			require(PreviousTaskCount > 0);
			if (bTerminalResultLifetimeCharged)
			{
				const uint64 PreviousResultCount = LifetimeAccounting->RetainedTerminalResultCount.fetch_sub(1, std::memory_order::acq_rel);
				require(PreviousResultCount > 0);
				if (Scope) Scope->ReleaseRetainedResult();
			}
		}

		auto RegisterDependent(const std::shared_ptr<FTaskStateData>& Dependent) -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() { return !IsTerminalState(State) || bTerminalPublicationFinished; });
			if (!IsTerminalState(State))
			{
				Dependents.emplace_back(Dependent);
			}
			return State;
		}

		auto OnPrerequisiteTerminal(ETaskState PrerequisiteState, uint64 PrerequisiteTaskId) -> void;
		auto TakeFunctionForQueue() -> std::unique_ptr<Private::FMoveOnlyTaskFunction>
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued)
			{
				return {};
			}
			DispatchTimeNanoseconds = MonotonicNanoseconds();
			return std::move(PendingFunction);
		}

		auto TryMarkRunning() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued)
			{
				return false;
			}
			State = ETaskState::Running;
			StartTimeNanoseconds = MonotonicNanoseconds();
			ExecutingThreadName = GetCurrentThreadName();
			if (FRunnableThread* CurrentThread = GetCurrentThread())
			{
				ExecutingThreadId = CurrentThread->GetThreadId();
			}
			return true;
		}

		auto MakeCancellationToken() -> FTaskCancellationToken
		{
			return FTaskCancellationToken(SharedCancellationState, weak_from_this());
		}

		auto IsCancellationRequested() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bCancellationRequested;
		}

		auto DependsOn(const FTaskStateData* PotentialPrerequisite) const -> bool
		{
			std::vector<std::shared_ptr<FTaskStateData>> PendingPrerequisites;
			{
				std::lock_guard Lock(Mutex);
				PendingPrerequisites.reserve(Prerequisites.size());
				for (const std::weak_ptr<FTaskStateData>& Prerequisite : Prerequisites)
				{
					if (std::shared_ptr<FTaskStateData> PinnedPrerequisite = Prerequisite.lock())
					{
						PendingPrerequisites.emplace_back(std::move(PinnedPrerequisite));
					}
				}
			}

			std::unordered_set<const FTaskStateData*> VisitedPrerequisites;
			while (!PendingPrerequisites.empty())
			{
				std::shared_ptr<FTaskStateData> Prerequisite = std::move(PendingPrerequisites.back());
				PendingPrerequisites.pop_back();
				if (Prerequisite.get() == PotentialPrerequisite)
				{
					return true;
				}
				if (!VisitedPrerequisites.emplace(Prerequisite.get()).second)
				{
					continue;
				}

				std::lock_guard Lock(Prerequisite->Mutex);
				for (const std::weak_ptr<FTaskStateData>& TransitivePrerequisite : Prerequisite->Prerequisites)
				{
					if (std::shared_ptr<FTaskStateData> PinnedPrerequisite = TransitivePrerequisite.lock())
					{
						PendingPrerequisites.emplace_back(std::move(PinnedPrerequisite));
					}
				}
			}
			return false;
		}

		auto RequestCancellation(std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::CancellationRequested, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto MarkSucceeded() -> void;
		auto MarkFailed(std::string InDiagnostic) -> void;
		auto MarkCanceled(std::string InDiagnostic) -> void;

		auto Wait() -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() {
				return IsTerminalState(State) && bTerminalPublicationFinished;
			});
			return State;
		}

		auto WaitFor(double TimeoutSeconds) -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), [this]() {
				return IsTerminalState(State) && bTerminalPublicationFinished;
			});
			return IsTerminalState(State) && !bTerminalPublicationFinished ? StateBeforeTerminal : State;
		}

		auto GetState() const -> ETaskState
		{
			std::lock_guard Lock(Mutex);
			return IsTerminalState(State) && !bTerminalPublicationFinished ? StateBeforeTerminal : State;
		}

		auto GetDiagnostic() const -> std::string
		{
			std::lock_guard Lock(Mutex);
			return IsTerminalState(State) && !bTerminalPublicationFinished ? std::string{} : Diagnostic;
		}

		auto GetDiagnostics() const -> FTaskDiagnostics
		{
			FTaskDiagnostics Snapshot;
			{
				std::lock_guard Lock(Mutex);
				Snapshot.TaskId = TaskId;
				Snapshot.ScopeId = Scope ? Scope->GetScopeId() : 0;
				Snapshot.ParentTaskId = ParentTaskId;
				Snapshot.PrerequisiteTaskIds = PrerequisiteTaskIds;
				Snapshot.PrerequisiteDependencyKinds = PrerequisiteDependencyKinds;
				const bool bTerminalVisible = !IsTerminalState(State) || bTerminalPublicationFinished;
				Snapshot.DirectBlockingTaskId = bTerminalVisible ? DirectBlockingTaskId : 0;
				Snapshot.EnqueueTimeNanoseconds = EnqueueTimeNanoseconds;
				Snapshot.DispatchTimeNanoseconds = DispatchTimeNanoseconds;
				Snapshot.QueueResidencyNanoseconds = DispatchTimeNanoseconds == 0
					? 0
					: (StartTimeNanoseconds == 0 ? MonotonicNanoseconds() : StartTimeNanoseconds) - DispatchTimeNanoseconds;
				Snapshot.StartTimeNanoseconds = StartTimeNanoseconds;
				Snapshot.FinishTimeNanoseconds = bTerminalVisible ? FinishTimeNanoseconds : 0;
				Snapshot.ExecutionNanoseconds = StartTimeNanoseconds == 0
					? 0
					: (!bTerminalVisible || FinishTimeNanoseconds == 0 ? MonotonicNanoseconds() : FinishTimeNanoseconds) - StartTimeNanoseconds;
				Snapshot.ExecutingThreadId = ExecutingThreadId;
				Snapshot.DebugName = DebugName;
				Snapshot.ExecutingThreadName = ExecutingThreadName;
				Snapshot.Diagnostic = bTerminalVisible ? Diagnostic : std::string{};
				Snapshot.State = bTerminalVisible ? State : StateBeforeTerminal;
				Snapshot.Target = Target;
				Snapshot.Priority = Priority;
				Snapshot.TerminalReason = bTerminalVisible ? TerminalReason : ETaskTerminalReason::None;
				Snapshot.EstimatedPayloadBytes = EstimatedPayloadBytes;
				Snapshot.EstimatedResultBytes = EstimatedResultBytes;
				Snapshot.RetainedResultBytes = bTerminalVisible ? RetainedResultBytes : 0;
				Snapshot.CallableStorageBytes = CallableStorageBytes;
				Snapshot.AttributionOwnerId = Private::FTaskAttributionAccess::GetOwnerId(Attribution);
				Snapshot.AttributionCategoryId = Private::FTaskAttributionAccess::GetCategoryId(Attribution);
				if (CoalescingKey)
				{
					Snapshot.CoalescingOwnerDomain = CoalescingKey->OwnerDomain;
					Snapshot.CoalescingWorkId = CoalescingKey->WorkId;
					Snapshot.CoalescingGeneration = CoalescingKey->Generation;
				}
				Snapshot.bHasResultStorage = bTerminalVisible && bHasResultStorage;
			}
			std::tie(Snapshot.AttributionOwner, Snapshot.AttributionCategory) = GTaskAttributionRegistry.Resolve(Attribution);
			return Snapshot;
		}

		auto SetRetainedResultBytes(uint64 InRetainedResultBytes) -> void;

		auto GetAccountingSnapshot() const -> FTaskAccountingSnapshot
		{
			std::lock_guard Lock(Mutex);
			return {
				.Attribution = Attribution,
				.State = State,
				.StateBeforeTerminal = StateBeforeTerminal,
				.TerminalReason = TerminalReason,
				.CallableBytes = CallableStorageBytes,
				.PayloadBytes = EstimatedPayloadBytes,
				.ResultBytes = EstimatedResultBytes,
				.RetainedResultBytes = RetainedResultBytes,
				.QueueResidencyNanoseconds = DispatchTimeNanoseconds == 0 || StartTimeNanoseconds == 0 ? 0 : StartTimeNanoseconds - DispatchTimeNanoseconds,
				.ExecutionNanoseconds = StartTimeNanoseconds == 0 || FinishTimeNanoseconds == 0 ? 0 : FinishTimeNanoseconds - StartTimeNanoseconds,
			};
		}

		auto GetDebugName() const -> const char* { return DebugName.c_str(); }
		auto GetTaskId() const -> uint64 { return TaskId; }
		auto PinScheduler() const -> std::shared_ptr<FTaskScheduler> { return Scheduler.lock(); }
		auto GetSharedCancellationState() const -> const std::shared_ptr<FTaskCancellationState>& { return SharedCancellationState; }
		auto GetTarget() const -> ETaskTarget { return Target; }
		auto GetPriority() const -> ETaskPriority { return Priority; }
		auto GetAttribution() const -> FTaskAttribution { return Attribution; }
		auto GetScope() const -> const std::shared_ptr<FTaskScopeState>& { return Scope; }
		auto GetScopeToken() const -> FTaskScopeToken { return FTaskScopeToken(Scope); }
		auto GetEstimatedPayloadBytes() const -> uint64 { return EstimatedPayloadBytes; }
		auto GetGenerationToken() const -> const FTaskGenerationToken& { return GenerationToken; }
		auto GetCoalescingKey() const -> const std::optional<FTaskCoalescingKey>& { return CoalescingKey; }

	private:
		auto PublishTerminal(ETaskState TerminalState, std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto PublishTerminalLocked(ETaskState TerminalState, std::string InDiagnostic, std::vector<std::shared_ptr<FTaskStateData>>& OutDependents, std::function<void(ETaskState)>& OutCompletionFunction, std::unique_ptr<Private::FMoveOnlyTaskFunction>& OutPendingFunction, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto FinishTerminalPublication(ETaskState TerminalState, std::vector<std::shared_ptr<FTaskStateData>>&& Dependents) -> void;

		uint64 TaskId = 0;
		uint64 ParentTaskId = 0;
		std::string DebugName;
		std::weak_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting;
		std::shared_ptr<FTaskCancellationState> SharedCancellationState;
		mutable std::mutex Mutex;
		std::condition_variable CV;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		std::function<void(ETaskState)> CompletionFunction;
		bool bHasResultStorage = false;
		std::vector<std::weak_ptr<FTaskStateData>> Prerequisites;
		std::vector<uint64> PrerequisiteTaskIds;
		std::vector<ETaskDependencyKind> PrerequisiteDependencyKinds;
		std::vector<std::weak_ptr<FTaskStateData>> Dependents;
		uint32 RemainingPrerequisites = 0;
		ETaskState State = ETaskState::Queued;
		ETaskState StateBeforeTerminal = ETaskState::Queued;
		ETaskDependencyKind DependencyKind = ETaskDependencyKind::Success;
		ETaskTerminalReason TerminalReason = ETaskTerminalReason::None;
		uint64 DirectBlockingTaskId = 0;
		uint64 BlockingPrerequisiteTaskId = 0;
		ETaskState BlockingPrerequisiteState = ETaskState::Succeeded;
		bool bAggregatePrerequisites = false;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		uint64 EstimatedPayloadBytes = 0;
		uint64 EstimatedResultBytes = 0;
		uint64 RetainedResultBytes = 0;
		FTaskAttribution Attribution;
		std::shared_ptr<FTaskScopeState> Scope;
		uint64 CallableStorageBytes = 0;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		bool bCancellationRequested = false;
		bool bTerminalPublicationFinished = false;
		bool bTerminalLifetimeCharged = false;
		bool bTerminalResultLifetimeCharged = false;
		std::string CancellationDiagnostic;
		ETaskTerminalReason CancellationReason = ETaskTerminalReason::CancellationRequested;
		uint64 CancellationDirectBlockingTaskId = 0;
		std::string Diagnostic;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 DispatchTimeNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string ExecutingThreadName;
	};

	auto FTaskScopeState::BindTask(const std::shared_ptr<FTaskStateData>& Task) -> bool
	{
		std::lock_guard Lock(Mutex);
		ActiveTasks.emplace(Task->GetTaskId(), Task);
		return State == ETaskScopeState::ClosingCancel || State == ETaskScopeState::QuiescentCancel;
	}

	auto FTaskScopeState::ReleaseTask(uint64 TaskId, ETaskState TerminalState) -> void
	{
		std::lock_guard Lock(Mutex);
		const size_t RemovedTaskCount = ActiveTasks.erase(TaskId);
		check(RemovedTaskCount == 1);
		require(CurrentActiveCount > 0);
		--CurrentActiveCount;
		if (TerminalState == ETaskState::Succeeded) ++SucceededCount;
		else if (TerminalState == ETaskState::Failed) ++FailedCount;
		else ++CanceledCount;
		if (CurrentActiveCount == 0)
		{
			if (State == ETaskScopeState::ClosingDrain) State = ETaskScopeState::QuiescentDrain;
			else if (State == ETaskScopeState::ClosingCancel) State = ETaskScopeState::QuiescentCancel;
			CV.notify_all();
		}
	}

	auto FTaskScopeState::Close(ETaskScopeCloseMode Mode) -> ETaskScopeCloseResult
	{
		std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
		ETaskScopeCloseResult Result = ETaskScopeCloseResult::Closed;
		{
			std::lock_guard Lock(Mutex);
			if (State == ETaskScopeState::QuiescentDrain || State == ETaskScopeState::QuiescentCancel)
			{
				return ETaskScopeCloseResult::AlreadyClosed;
			}
			if (State == ETaskScopeState::ClosingCancel || (State == ETaskScopeState::ClosingDrain && Mode == ETaskScopeCloseMode::Drain))
			{
				return ETaskScopeCloseResult::AlreadyClosed;
			}
			if (State == ETaskScopeState::ClosingDrain)
			{
				State = ETaskScopeState::ClosingCancel;
				Result = ETaskScopeCloseResult::EscalatedToCancel;
			}
			else
			{
				State = Mode == ETaskScopeCloseMode::Cancel ? ETaskScopeState::ClosingCancel : ETaskScopeState::ClosingDrain;
			}
			if (CurrentActiveCount == 0)
			{
				State = State == ETaskScopeState::ClosingCancel ? ETaskScopeState::QuiescentCancel : ETaskScopeState::QuiescentDrain;
				CV.notify_all();
			}
			else if (State == ETaskScopeState::ClosingCancel)
			{
				TasksToCancel.reserve(ActiveTasks.size());
				for (const auto& [TaskId, Task] : ActiveTasks)
				{
					if (std::shared_ptr<FTaskStateData> PinnedTask = Task.lock()) TasksToCancel.emplace_back(std::move(PinnedTask));
				}
			}
		}
		for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
		{
			Task->RequestCancellation("Task scope cancellation was requested.");
		}
		return Result;
	}

	auto FTaskScopeState::AbandonOpen() -> void
	{
		std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskScopeState::Open)
			{
				return;
			}
			State = CurrentActiveCount == 0 ? ETaskScopeState::QuiescentCancel : ETaskScopeState::ClosingCancel;
			SchedulerAccounting->AbandonedOpenScopeCount.fetch_add(1, std::memory_order::acq_rel);
			if (CurrentActiveCount == 0)
			{
				CV.notify_all();
			}
			else
			{
				TasksToCancel.reserve(ActiveTasks.size());
				for (const auto& [TaskId, Task] : ActiveTasks)
				{
					if (std::shared_ptr<FTaskStateData> PinnedTask = Task.lock()) TasksToCancel.emplace_back(std::move(PinnedTask));
				}
			}
		}
		for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
		{
			Task->RequestCancellation("Task scope controller was destroyed while open.");
		}
	}

	auto FTaskScopeState::GetDiagnostics() const -> FTaskScopeDiagnostics
	{
		FTaskScopeDiagnostics Snapshot;
		std::vector<std::shared_ptr<FTaskStateData>> Tasks;
		{
			std::lock_guard Lock(Mutex);
			Snapshot.State = State;
			Snapshot.ScopeId = ScopeId;
			Snapshot.AcceptedCount = AcceptedCount;
			Snapshot.RejectedCount = RejectedCount;
			Snapshot.SucceededCount = SucceededCount;
			Snapshot.FailedCount = FailedCount;
			Snapshot.CanceledCount = CanceledCount;
			Snapshot.CurrentActiveCount = CurrentActiveCount;
			Snapshot.PeakActiveCount = PeakActiveCount;
			Snapshot.CurrentRetainedResultCount = CurrentRetainedResultCount;
			Snapshot.PeakRetainedResultCount = PeakRetainedResultCount;
			Tasks.reserve(std::min<size_t>(64, ActiveTasks.size()));
			for (const auto& [TaskId, Task] : ActiveTasks)
			{
				if (std::shared_ptr<FTaskStateData> PinnedTask = Task.lock()) Tasks.emplace_back(std::move(PinnedTask));
			}
		}
		std::ranges::sort(Tasks, {}, &FTaskStateData::GetTaskId);
		if (Tasks.size() > 64)
		{
			Snapshot.NonterminalSnapshotTruncationCount = Tasks.size() - 64;
			Tasks.resize(64);
		}
		Snapshot.NonterminalTasks.reserve(Tasks.size());
		for (const std::shared_ptr<FTaskStateData>& Task : Tasks)
		{
			FTaskDiagnostics Diagnostics = Task->GetDiagnostics();
			if (!IsTerminalState(Diagnostics.State)) Snapshot.NonterminalTasks.emplace_back(std::move(Diagnostics));
		}
		return Snapshot;
	}

	FTaskScope::~FTaskScope()
	{
		if (State) State->AbandonOpen();
	}

	FTaskScope::FTaskScope(FTaskScope&& Other) noexcept
		: State(std::move(Other.State))
	{
	}

	auto FTaskScope::operator=(FTaskScope&& Other) noexcept -> FTaskScope&
	{
		if (this == &Other) return *this;
		if (State) State->AbandonOpen();
		State = std::move(Other.State);
		return *this;
	}

	auto FTaskScope::IsValid() const -> bool { return static_cast<bool>(State); }
	auto FTaskScope::GetToken() const -> FTaskScopeToken { return FTaskScopeToken(State); }
	auto FTaskScope::Close(ETaskScopeCloseMode Mode) -> ETaskScopeCloseResult
	{
		return State ? State->Close(Mode) : ETaskScopeCloseResult::Invalid;
	}
	auto FTaskScope::Wait() const -> ETaskScopeWaitResult
	{
		return State ? State->WaitFor(0.0, true) : ETaskScopeWaitResult::Invalid;
	}
	auto FTaskScope::WaitFor(double TimeoutSeconds) const -> ETaskScopeWaitResult
	{
		return State ? State->WaitFor(TimeoutSeconds, false) : ETaskScopeWaitResult::Invalid;
	}
	auto FTaskScope::GetDiagnostics() const -> FTaskScopeDiagnostics
	{
		return State ? State->GetDiagnostics() : FTaskScopeDiagnostics{};
	}

	auto DispatchGameThreadDeferredTask(
		const std::shared_ptr<FTaskStateData>& State,
		Private::FMoveOnlyTaskFunction&& Function) -> bool;

	// Owns the process scheduler's pool and every accepted nonterminal node.
	class FTaskScheduler final : public std::enable_shared_from_this<FTaskScheduler>
	{
	public:
		static auto Create(const FTaskSchedulerConfig& Config) -> std::shared_ptr<FTaskScheduler>
		{
			auto Scheduler = std::shared_ptr<FTaskScheduler>(new FTaskScheduler());
			const uint32 NumThreads = Config.NumWorkerThreads > 0 ? Config.NumWorkerThreads : GetDefaultThreadPoolThreadCount();
			if (!Scheduler->Pool.Create(NumThreads, "EngineWorker"))
			{
				return nullptr;
			}

			Scheduler->WorkerCount = NumThreads;
			Scheduler->TaskReservationCapacity = Config.MaxNonterminalTasks;
			Scheduler->bAcceptingTasks = true;
			return Scheduler;
		}

		auto GetAggregate(FTaskAttribution Attribution) -> FTaskOwnerCategoryAggregate&
		{
			const uint16 CategoryId = Private::FTaskAttributionAccess::GetCategoryId(Attribution);
			return OwnerCategoryAggregates[CategoryId < MaxTaskAttributionPairs ? CategoryId : OverflowAttributionId];
		}

		auto RecordAcceptedTask(const FTaskAccountingSnapshot& Task) -> void
		{
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Task.Attribution);
			Aggregate.Increment(ETaskAggregateCounter::Accepted);
			Aggregate.Add(ETaskAggregateGauge::Nonterminal, 1);
			Aggregate.Add(Task.State == ETaskState::Waiting ? ETaskAggregateGauge::Waiting : ETaskAggregateGauge::Queued, 1);
			Aggregate.Add(ETaskAggregateGauge::CallableBytes, Task.CallableBytes);
			Aggregate.Add(ETaskAggregateGauge::PayloadBytes, Task.PayloadBytes);
			Aggregate.Add(ETaskAggregateGauge::ResultBytes, Task.ResultBytes);
			Aggregate.Record(ETaskAggregateHistogram::CallableBytes, Task.CallableBytes);
			Aggregate.Record(ETaskAggregateHistogram::PayloadBytes, Task.PayloadBytes);
			Aggregate.Record(ETaskAggregateHistogram::ResultBytes, Task.ResultBytes);
		}

		auto OnTaskQueued(FTaskAttribution Attribution) -> void
		{
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Attribution);
			Aggregate.Subtract(ETaskAggregateGauge::Waiting, 1);
			Aggregate.Add(ETaskAggregateGauge::Queued, 1);
		}

		auto OnTaskStarted(const FTaskAccountingSnapshot& Task) -> void
		{
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Task.Attribution);
			Aggregate.Subtract(ETaskAggregateGauge::Queued, 1);
			Aggregate.Add(ETaskAggregateGauge::Running, 1);
			Aggregate.Record(ETaskAggregateHistogram::QueueResidency, Task.QueueResidencyNanoseconds);
		}

		auto OnRetainedResultBytesChanged(FTaskAttribution Attribution, uint64 PreviousBytes, uint64 CurrentBytes) -> void
		{
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Attribution);
			if (CurrentBytes > PreviousBytes)
			{
				Aggregate.Add(ETaskAggregateGauge::RetainedUniqueResultBytes, CurrentBytes - PreviousBytes);
			}
			else
			{
				Aggregate.Subtract(ETaskAggregateGauge::RetainedUniqueResultBytes, PreviousBytes - CurrentBytes);
			}
		}

		auto RecordParallelForOperation(FTaskAttribution Attribution) -> void
		{
			GetAggregate(Attribution).Increment(ETaskAggregateCounter::ParallelForOperation);
		}

		auto GetWorkerCount() const -> uint32 { return WorkerCount; }
		auto CreateScope() -> std::shared_ptr<FTaskScopeState>
		{
			auto Scope = std::make_shared<FTaskScopeState>(weak_from_this(), ScopeAccounting);
			RegisterScope(Scope);
			return Scope;
		}

		auto RegisterScope(const std::shared_ptr<FTaskScopeState>& Scope) -> void
		{
			std::lock_guard Lock(Mutex);
			std::erase_if(LiveScopes, [](const std::weak_ptr<FTaskScopeState>& Entry) { return Entry.expired(); });
			LiveScopes.emplace_back(Scope);
		}

		auto CloseLiveScopes(ETaskScopeCloseMode Mode) -> void
		{
			std::vector<std::shared_ptr<FTaskScopeState>> Scopes;
			{
				std::lock_guard Lock(Mutex);
				for (auto Iterator = LiveScopes.begin(); Iterator != LiveScopes.end();)
				{
					if (std::shared_ptr<FTaskScopeState> Scope = Iterator->lock())
					{
						Scopes.emplace_back(std::move(Scope));
						++Iterator;
					}
					else
					{
						Iterator = LiveScopes.erase(Iterator);
					}
				}
			}
			for (const std::shared_ptr<FTaskScopeState>& Scope : Scopes) Scope->Close(Mode);
		}

		auto SnapshotScopeCounts(uint64& OutLive, uint64& OutOpen, uint64& OutNonquiescent) -> void
		{
			std::vector<std::shared_ptr<FTaskScopeState>> Scopes;
			{
				std::lock_guard Lock(Mutex);
				for (auto Iterator = LiveScopes.begin(); Iterator != LiveScopes.end();)
				{
					if (std::shared_ptr<FTaskScopeState> Scope = Iterator->lock())
					{
						Scopes.emplace_back(std::move(Scope));
						++Iterator;
					}
					else
					{
						Iterator = LiveScopes.erase(Iterator);
					}
				}
			}
			OutLive = Scopes.size();
			OutOpen = 0;
			OutNonquiescent = 0;
			for (const std::shared_ptr<FTaskScopeState>& Scope : Scopes)
			{
				bool bOpen = false;
				bool bNonquiescent = false;
				Scope->GetRegistryState(bOpen, bNonquiescent);
				OutOpen += bOpen ? 1 : 0;
				OutNonquiescent += bNonquiescent ? 1 : 0;
			}
		}

		auto Submit(
			const char* Name,
			std::unique_ptr<Private::FMoveOnlyTaskFunction>& FunctionOwner,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			ETaskDependencyKind DependencyKind = ETaskDependencyKind::Success,
			bool bAggregatePrerequisites = false,
			ETaskTarget Target = ETaskTarget::AnyWorker,
			ETaskPriority Priority = ETaskPriority::Normal,
			uint64 EstimatedPayloadBytes = 0,
			uint64 EstimatedResultBytes = 0,
			FTaskGenerationToken GenerationToken = {},
			std::optional<FTaskCoalescingKey> CoalescingKey = {},
			bool bScopeSelectedByPrimaryPredecessor = false) -> std::shared_ptr<FTaskStateData>
		{
			std::shared_ptr<FTaskStateData> State;
			std::shared_ptr<FTaskScopeState> SelectedScope;
			std::vector<std::shared_ptr<FTaskStateData>> PrerequisiteStates;
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingTasks)
				{
					RecordRejectedTask(Options.Attribution, FunctionOwner ? FunctionOwner->GetStorageBytes() : 0);
					return {};
				}

				for (const FTaskHandle& Prerequisite : Options.Prerequisites)
				{
					if (!Prerequisite.State || Prerequisite.State->PinScheduler().get() != this)
					{
						RecordRejectedTask(Options.Attribution, FunctionOwner ? FunctionOwner->GetStorageBytes() : 0);
						return {};
					}
					PrerequisiteStates.emplace_back(Prerequisite.State);
				}
				SelectedScope = Options.Scope.State;
				if (!bScopeSelectedByPrimaryPredecessor && GCurrentTaskState && GCurrentTaskScheduler == this)
				{
					const std::shared_ptr<FTaskScopeState>& InheritedScope = GCurrentTaskState->GetScope();
					if (InheritedScope && SelectedScope && SelectedScope != InheritedScope)
					{
						SelectedScope->RecordRejected();
						ScopeAccounting->RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
						RecordRejectedTask(Options.Attribution, FunctionOwner ? FunctionOwner->GetStorageBytes() : 0);
						return {};
					}
					if (InheritedScope) SelectedScope = InheritedScope;
				}
				if (CurrentTaskReservationCount.load(std::memory_order::acquire) >= TaskReservationCapacity)
				{
					RecordCapacityRejectedTask(Options.Attribution, FunctionOwner ? FunctionOwner->GetStorageBytes() : 0);
					return {};
				}
				const uint64 CurrentReservations = CurrentTaskReservationCount.fetch_add(1, std::memory_order::acq_rel) + 1;
				if (SelectedScope && !SelectedScope->TryAdmit(this))
				{
					ScopeAccounting->RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
					const uint64 PreviousReservationCount = CurrentTaskReservationCount.fetch_sub(1, std::memory_order::acq_rel);
					require(PreviousReservationCount > 0);
					RecordRejectedTask(Options.Attribution, FunctionOwner ? FunctionOwner->GetStorageBytes() : 0);
					return {};
				}
				uint64 PeakReservations = PeakTaskReservationCount.load(std::memory_order::acquire);
				while (PeakReservations < CurrentReservations
					&& !PeakTaskReservationCount.compare_exchange_weak(PeakReservations, CurrentReservations, std::memory_order::acq_rel)) {}

				try
				{
					State = std::make_shared<FTaskStateData>(
						Name,
						weak_from_this(),
						LifetimeAccounting,
						std::move(FunctionOwner),
						std::move(CompletionFunction),
						Options.CancellationToken,
						PrerequisiteStates,
						GCurrentTaskState ? GCurrentTaskState->GetTaskId() : 0,
						DependencyKind,
						bAggregatePrerequisites,
						Target,
						Priority,
						EstimatedPayloadBytes,
						EstimatedResultBytes,
						Options.Attribution,
						SelectedScope,
						std::move(GenerationToken),
						std::move(CoalescingKey)
					);
				}
				catch (...)
				{
					const uint64 PreviousReservationCount = CurrentTaskReservationCount.fetch_sub(1, std::memory_order::acq_rel);
					require(PreviousReservationCount > 0);
					if (SelectedScope) SelectedScope->RollbackAdmission();
					throw;
				}
				ActiveTasks.emplace(State->GetTaskId(), State);
			}
			RecordAcceptedTask(State->GetAccountingSnapshot());
			Profiling::TaskEnqueued(
				State->GetTaskId(),
				State->GetScope() ? State->GetScope()->GetScopeId() : 0,
				Private::FTaskAttributionAccess::GetOwnerId(State->GetAttribution()),
				Private::FTaskAttributionAccess::GetCategoryId(State->GetAttribution()),
				static_cast<uint8>(State->GetTarget()));

			if (const std::shared_ptr<FTaskCancellationState>& CancellationState = State->GetSharedCancellationState())
			{
				CancellationState->RegisterTask(State);
			}

			const bool bCancelAfterAdmission = SelectedScope && SelectedScope->BindTask(State);
			for (const std::shared_ptr<FTaskStateData>& Prerequisite : PrerequisiteStates)
			{
				const ETaskState PrerequisiteState = Prerequisite->RegisterDependent(State);
				if (IsTerminalState(PrerequisiteState))
				{
					State->OnPrerequisiteTerminal(PrerequisiteState, Prerequisite->GetTaskId());
				}
			}

			if (bCancelAfterAdmission)
			{
				State->RequestCancellation("Task scope was canceled during admission.");
			}
			else if (Options.Prerequisites.empty())
			{
				QueueTask(State);
			}

			DURIN_TRACE("Task accepted. (task: {}, id: {}, prerequisites: {})", State->GetDebugName(), State->GetTaskId(), Options.Prerequisites.size());
			return State;
		}

		auto QueueTask(const std::shared_ptr<FTaskStateData>& State) -> void
		{
			std::unique_ptr<Private::FMoveOnlyTaskFunction> FunctionOwner = State->TakeFunctionForQueue();
			if (!FunctionOwner)
			{
				return;
			}
			Private::FMoveOnlyTaskFunction Function = std::move(*FunctionOwner);
			if (State->GetTarget() == ETaskTarget::GameThreadDeferred)
			{
				if (!DispatchGameThreadDeferredTask(State, std::move(Function)))
				{
					RecordRejectedTask(State->GetAttribution());
					State->RequestCancellation("GameThread deferred executor rejected task dispatch.", ETaskTerminalReason::DispatchRejected);
				}
				return;
			}

			const bool bAccepted = Pool.Enqueue(
				State->GetDebugName(),
				[State, Scheduler = this, Function = std::move(Function)]() mutable {
					Scheduler->ExecuteTask(State, std::move(Function), true);
				},
				[State]() {
					State->RequestCancellation("Task was discarded during scheduler shutdown.", ETaskTerminalReason::ShutdownCanceled);
				},
				State->GetScope() ? State->GetScope()->GetScopeId() : 0
			);

			if (!bAccepted)
			{
				RecordRejectedTask(State->GetAttribution());
				State->RequestCancellation("Task could not be queued because scheduler shutdown had begun.", ETaskTerminalReason::DispatchRejected);
			}
		}

		auto ExecuteTask(
			const std::shared_ptr<FTaskStateData>& State,
			Private::FMoveOnlyTaskFunction&& Function,
			bool bWorkerExecution) -> void
		{
			if (!State->TryMarkRunning())
			{
				Function = {};
				return;
			}
			OnTaskStarted(State->GetAccountingSnapshot());
			if (bWorkerExecution) OnWorkerStarted();
			DURIN_PROFILE_TASK_EXECUTION_ZONE(
				State->GetDebugName(),
				State->GetTaskId(),
				State->GetScope() ? State->GetScope()->GetScopeId() : 0,
				Private::FTaskAttributionAccess::GetOwnerId(State->GetAttribution()),
				Private::FTaskAttributionAccess::GetCategoryId(State->GetAttribution()),
				static_cast<uint8>(State->GetTarget()));

			FTaskStateData* PreviousTaskState = GCurrentTaskState;
			FTaskScheduler* PreviousTaskScheduler = GCurrentTaskScheduler;
			GCurrentTaskState = State.get();
			GCurrentTaskScheduler = this;

			try
			{
				Function(State->MakeCancellationToken());
				Function = {};
				State->MarkSucceeded();
			}
			catch (const std::exception& Exception)
			{
				Function = {};
				State->MarkFailed(Exception.what());
			}
			catch (...)
			{
				Function = {};
				State->MarkFailed("Task callable threw an unknown exception.");
			}

			GCurrentTaskState = PreviousTaskState;
			GCurrentTaskScheduler = PreviousTaskScheduler;
			if (bWorkerExecution) OnWorkerFinished();
		}

		auto CloseAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAcceptingTasks = false;
		}

		auto Shutdown(bool bWaitForQueuedWork) -> void
		{
			CloseAdmission();
			if (!bWaitForQueuedWork)
			{
				std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
				{
					std::lock_guard Lock(Mutex);
					TasksToCancel.reserve(ActiveTasks.size());
					for (const auto& [TaskId, Task] : ActiveTasks)
					{
						TasksToCancel.push_back(Task);
					}
				}
				for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
				{
					Task->RequestCancellation("Task was canceled during scheduler shutdown.", ETaskTerminalReason::ShutdownCanceled);
				}

				Pool.Destroy(false);
			}

			{
				std::unique_lock Lock(Mutex);
				if (!QuiescenceCV.wait_for(Lock, std::chrono::duration<double>(LongWaitThresholdSeconds), [this]() {
					return ActiveTasks.empty();
				}))
				{
					std::vector<std::shared_ptr<FTaskStateData>> NonterminalTasks;
					NonterminalTasks.reserve(ActiveTasks.size());
					for (const auto& [TaskId, Task] : ActiveTasks)
					{
						NonterminalTasks.emplace_back(Task);
					}
					LongWaitCount.fetch_add(1, std::memory_order::acq_rel);
					Lock.unlock();
					for (const std::shared_ptr<FTaskStateData>& Task : NonterminalTasks)
					{
						const FTaskDiagnostics Diagnostics = Task->GetDiagnostics();
						DURIN_WARN(
							"Task scheduler shutdown is waiting for a nonterminal task. (task: {}, id: {}, state: {})",
							Diagnostics.DebugName,
							Diagnostics.TaskId,
							static_cast<uint32>(Diagnostics.State)
						);
					}
					Lock.lock();
					QuiescenceCV.wait(Lock, [this]() {
						return ActiveTasks.empty();
					});
				}
			}

			if (bWaitForQueuedWork)
			{
				Pool.Destroy(true);
			}
		}

		auto OnTaskTerminal(const std::shared_ptr<FTaskStateData>& State) -> void
		{
			const FTaskAccountingSnapshot Task = State->GetAccountingSnapshot();
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Task.Attribution);
			Aggregate.Subtract(ETaskAggregateGauge::Nonterminal, 1);
			if (Task.StateBeforeTerminal == ETaskState::Waiting) Aggregate.Subtract(ETaskAggregateGauge::Waiting, 1);
			else if (Task.StateBeforeTerminal == ETaskState::Queued) Aggregate.Subtract(ETaskAggregateGauge::Queued, 1);
			else if (Task.StateBeforeTerminal == ETaskState::Running) Aggregate.Subtract(ETaskAggregateGauge::Running, 1);
			Aggregate.Subtract(ETaskAggregateGauge::CallableBytes, Task.CallableBytes);
			Aggregate.Subtract(ETaskAggregateGauge::PayloadBytes, Task.PayloadBytes);
			Aggregate.Subtract(ETaskAggregateGauge::ResultBytes, Task.ResultBytes);
			if (Task.State == ETaskState::Succeeded) Aggregate.Increment(ETaskAggregateCounter::Succeeded);
			else if (Task.State == ETaskState::Failed) Aggregate.Increment(ETaskAggregateCounter::Failed);
			else Aggregate.Increment(ETaskAggregateCounter::Canceled);
			switch (Task.TerminalReason)
			{
			case ETaskTerminalReason::DependencyFailed: Aggregate.Increment(ETaskAggregateCounter::DependencyFailed); break;
			case ETaskTerminalReason::DependencyCanceled: Aggregate.Increment(ETaskAggregateCounter::DependencyCanceled); break;
			case ETaskTerminalReason::CancellationRequested: Aggregate.Increment(ETaskAggregateCounter::CancellationRequested); break;
			case ETaskTerminalReason::DispatchRejected: Aggregate.Increment(ETaskAggregateCounter::DispatchRejected); break;
			case ETaskTerminalReason::CapacityExhausted: Aggregate.Increment(ETaskAggregateCounter::CapacityExhausted); break;
			case ETaskTerminalReason::Superseded: Aggregate.Increment(ETaskAggregateCounter::Superseded); break;
			case ETaskTerminalReason::StaleGeneration: Aggregate.Increment(ETaskAggregateCounter::StaleGeneration); break;
			case ETaskTerminalReason::CallbackFailure: Aggregate.Increment(ETaskAggregateCounter::CallbackFailure); break;
			case ETaskTerminalReason::ShutdownCanceled: Aggregate.Increment(ETaskAggregateCounter::ShutdownCanceled); break;
			default: break;
			}
			if (Task.StateBeforeTerminal == ETaskState::Running)
			{
				Aggregate.Record(ETaskAggregateHistogram::Execution, Task.ExecutionNanoseconds);
			}
			Profiling::TaskTerminal(
				State->GetTaskId(),
				State->GetScope() ? State->GetScope()->GetScopeId() : 0,
				Private::FTaskAttributionAccess::GetOwnerId(Task.Attribution),
				Private::FTaskAttributionAccess::GetCategoryId(Task.Attribution),
				static_cast<uint8>(State->GetTarget()),
				static_cast<uint8>(Task.TerminalReason));

			std::lock_guard Lock(Mutex);
			const size_t RemovedTaskCount = ActiveTasks.erase(State->GetTaskId());
			check(RemovedTaskCount == 1);
			const uint64 PreviousReservationCount = CurrentTaskReservationCount.fetch_sub(1, std::memory_order::acq_rel);
			require(PreviousReservationCount > 0);
			CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			if (Task.State == ETaskState::Failed)
			{
				FailedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}
			else if (Task.State == ETaskState::Canceled)
			{
				CanceledTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}
			QuiescenceCV.notify_all();
		}

		auto TryExecuteOneQueuedTask() -> bool
		{
			return Pool.TryExecuteOneQueuedTask();
		}

		auto GetActiveTaskCount() const -> uint64
		{
			std::lock_guard Lock(Mutex);
			return ActiveTasks.size();
		}

		auto WaitForGraphProgress(double TimeoutSeconds) -> void
		{
			std::unique_lock Lock(Mutex);
			QuiescenceCV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds));
		}

		auto RecordShutdownLongWait() -> void
		{
			LongWaitCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto RecordRejectedTask(FTaskAttribution Attribution = {}, std::optional<uint64> CallableBytes = {}) -> void
		{
			RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Attribution);
			Aggregate.Increment(ETaskAggregateCounter::Rejected);
			if (CallableBytes) Aggregate.Record(ETaskAggregateHistogram::CallableBytes, *CallableBytes);
		}

		auto RecordScopeRejectedTask() -> void
		{
			ScopeAccounting->RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto RecordCapacityRejectedTask(FTaskAttribution Attribution, std::optional<uint64> CallableBytes = {}) -> void
		{
			RecordRejectedTask(Attribution, CallableBytes);
			CapacityRejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			GetAggregate(Attribution).Increment(ETaskAggregateCounter::CapacityExhausted);
		}

		auto RecordDuplicateUniqueConsumerClaim() -> void
		{
			DuplicateUniqueConsumerClaimCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto RecordLongWait(const char* WaiterName, const FTaskDiagnostics& Target, uint64 ElapsedNanoseconds) -> void
		{
			std::lock_guard Lock(Mutex);
			LastLongWaiterName = WaiterName ? WaiterName : "Unknown";
			LastLongWaitTargetName = Target.DebugName;
			LastLongWaitTargetTaskId = Target.TaskId;
			LastLongWaitTargetState = Target.State;
			LastLongWaitElapsedNanoseconds = ElapsedNanoseconds;
			LongWaitCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto OnWorkerStarted() -> void
		{
			ActiveWorkerCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto OnWorkerFinished() -> void
		{
			const uint32 PreviousCount = ActiveWorkerCount.fetch_sub(1, std::memory_order::acq_rel);
			check(PreviousCount > 0);
		}

		auto PublishProfilerPlots() -> void
		{
			std::vector<FTaskOwnerCategoryDiagnostics> Entries = GTaskAttributionRegistry.Snapshot();
			for (FTaskOwnerCategoryDiagnostics& Entry : Entries)
			{
				OwnerCategoryAggregates[Entry.CategoryId].Snapshot(Entry);
				Profiling::TaskAggregatePlots(
					Entry.OwnerId,
					Entry.CategoryId,
					Entry.CurrentWaitingCount + Entry.CurrentQueuedCount,
					Entry.CurrentRunningCount,
					Entry.RejectedCount,
					Entry.CurrentCallableBytes,
					Entry.CurrentPayloadBytes,
					Entry.CurrentResultBytes,
					Entry.CurrentRetainedUniqueResultBytes);
			}
		}

		auto GetDiagnostics(bool bInRunning) -> FTaskSchedulerDiagnostics
		{
			FTaskSchedulerDiagnostics Snapshot;
			Snapshot.WorkerCount = WorkerCount;
			Snapshot.TaskReservationCapacity = TaskReservationCapacity;
			Snapshot.CurrentTaskReservationCount = CurrentTaskReservationCount.load(std::memory_order::acquire);
			Snapshot.PeakTaskReservationCount = PeakTaskReservationCount.load(std::memory_order::acquire);
			Snapshot.QueueDepth = Pool.GetNumQueuedTasks();
			Snapshot.ActiveWorkerCount = ActiveWorkerCount.load(std::memory_order::acquire);
			Snapshot.CompletedTaskCount = CompletedTaskCount.load(std::memory_order::acquire);
			Snapshot.FailedTaskCount = FailedTaskCount.load(std::memory_order::acquire);
			Snapshot.CanceledTaskCount = CanceledTaskCount.load(std::memory_order::acquire);
			Snapshot.RejectedTaskCount = RejectedTaskCount.load(std::memory_order::acquire);
			Snapshot.CapacityRejectedTaskCount = CapacityRejectedTaskCount.load(std::memory_order::acquire);
			Snapshot.AbandonedOpenScopeCount = ScopeAccounting->AbandonedOpenScopeCount.load(std::memory_order::acquire);
			Snapshot.ScopeRejectedTaskCount = ScopeAccounting->RejectedTaskCount.load(std::memory_order::acquire);
			SnapshotScopeCounts(Snapshot.LiveScopeCount, Snapshot.OpenScopeCount, Snapshot.NonquiescentScopeCount);
			Snapshot.LongWaitCount = LongWaitCount.load(std::memory_order::acquire);
			Snapshot.DuplicateUniqueConsumerClaimCount = DuplicateUniqueConsumerClaimCount.load(std::memory_order::acquire);
			Snapshot.AttributionRegistrationOverflowCount = GTaskAttributionRegistry.GetOverflowCount();
			Snapshot.OwnerCategoryDiagnostics = GTaskAttributionRegistry.Snapshot();
			for (FTaskOwnerCategoryDiagnostics& Entry : Snapshot.OwnerCategoryDiagnostics)
			{
				OwnerCategoryAggregates[Entry.CategoryId].Snapshot(Entry);
				Snapshot.RetainedUniqueResultBytes += Entry.CurrentRetainedUniqueResultBytes;
			}
			Snapshot.bRunning = bInRunning;

			std::vector<std::shared_ptr<FTaskStateData>> ActiveTaskSnapshot;
			{
				std::lock_guard Lock(Mutex);
				Snapshot.LastLongWaiterName = LastLongWaiterName;
				Snapshot.LastLongWaitTargetName = LastLongWaitTargetName;
				Snapshot.LastLongWaitTargetTaskId = LastLongWaitTargetTaskId;
				Snapshot.LastLongWaitTargetState = LastLongWaitTargetState;
				Snapshot.LastLongWaitElapsedNanoseconds = LastLongWaitElapsedNanoseconds;
				ActiveTaskSnapshot.reserve(ActiveTasks.size());
				for (const auto& [TaskId, Task] : ActiveTasks)
				{
					ActiveTaskSnapshot.emplace_back(Task);
				}
			}
			if (GTaskSchedulerSnapshotTestHookEnabled.load(std::memory_order::acquire))
			{
				std::function<void()> Hook;
				{
					std::lock_guard Lock(GTaskSchedulerSnapshotTestHookMutex);
					Hook = GTaskSchedulerSnapshotTestHook;
				}
				if (Hook) Hook();
			}
			Snapshot.NonterminalTasks.reserve(ActiveTaskSnapshot.size());
			for (const std::shared_ptr<FTaskStateData>& Task : ActiveTaskSnapshot)
			{
				FTaskDiagnostics TaskSnapshot = Task->GetDiagnostics();
				if (!IsTerminalState(TaskSnapshot.State))
				{
					Snapshot.NonterminalTasks.emplace_back(std::move(TaskSnapshot));
				}
			}
			Snapshot.NonterminalTaskCount = Snapshot.NonterminalTasks.size();
			Snapshot.RetainedTerminalHandleCount = LifetimeAccounting->RetainedTerminalTaskCount.load(std::memory_order::acquire);
			Snapshot.RetainedTerminalResultCount = LifetimeAccounting->RetainedTerminalResultCount.load(std::memory_order::acquire);
			return Snapshot;
		}

		auto GetLifetimeAccounting() const -> const std::shared_ptr<FTaskSchedulerLifetimeAccounting>& { return LifetimeAccounting; }
		auto WaitForScopeWorkerCallables(uint64 ScopeId, double TimeoutSeconds) -> bool
		{
			return Pool.WaitForOwnerTagIdle(ScopeId, TimeoutSeconds);
		}
		auto GetScopeWorkerCallableCount(uint64 ScopeId) const -> uint32
		{
			return Pool.GetOwnerTagOutstandingCount(ScopeId);
		}

	private:
		FQueuedThreadPool Pool;
		mutable std::mutex Mutex;
		std::condition_variable QuiescenceCV;
		std::unordered_map<uint64, std::shared_ptr<FTaskStateData>> ActiveTasks;
		std::vector<std::weak_ptr<FTaskScopeState>> LiveScopes;
		std::shared_ptr<FTaskScopeSchedulerAccounting> ScopeAccounting = std::make_shared<FTaskScopeSchedulerAccounting>();
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting = std::make_shared<FTaskSchedulerLifetimeAccounting>();
		std::array<FTaskOwnerCategoryAggregate, MaxTaskAttributionPairs> OwnerCategoryAggregates{};
		std::atomic<uint32> ActiveWorkerCount = 0;
		std::atomic<uint64> CompletedTaskCount = 0;
		std::atomic<uint64> FailedTaskCount = 0;
		std::atomic<uint64> CanceledTaskCount = 0;
		std::atomic<uint64> RejectedTaskCount = 0;
		std::atomic<uint64> CapacityRejectedTaskCount = 0;
		std::atomic<uint64> CurrentTaskReservationCount = 0;
		std::atomic<uint64> PeakTaskReservationCount = 0;
		std::atomic<uint64> LongWaitCount = 0;
		std::atomic<uint64> DuplicateUniqueConsumerClaimCount = 0;
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		uint32 WorkerCount = 0;
		uint64 TaskReservationCapacity = 0;
		bool bAcceptingTasks = false;
	};

	auto FTaskScopeState::WaitFor(double TimeoutSeconds, bool bUnbounded) -> ETaskScopeWaitResult
	{
		std::vector<std::shared_ptr<FTaskStateData>> Tasks;
		{
			std::lock_guard Lock(Mutex);
			if (State == ETaskScopeState::Open) return ETaskScopeWaitResult::ScopeOpen;
			if (State == ETaskScopeState::QuiescentDrain || State == ETaskScopeState::QuiescentCancel)
			{
				return ETaskScopeWaitResult::Quiescent;
			}
			Tasks.reserve(ActiveTasks.size());
			for (const auto& [TaskId, Task] : ActiveTasks)
			{
				if (std::shared_ptr<FTaskStateData> PinnedTask = Task.lock()) Tasks.emplace_back(std::move(PinnedTask));
			}
		}

		if (IsInRenderingThread()) return ETaskScopeWaitResult::UnsupportedThread;
		if (GIsGameThreadIdInitialized && IsInGameThread() && std::ranges::any_of(Tasks, [](const std::shared_ptr<FTaskStateData>& Task) {
			return Task->GetTarget() == ETaskTarget::GameThreadDeferred;
		}))
		{
			return ETaskScopeWaitResult::UnsupportedThread;
		}

		std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock();
		const bool bWorkerHelping = GCurrentTaskState && PinnedScheduler && GCurrentTaskScheduler == PinnedScheduler.get();
		if (bWorkerHelping && GCurrentTaskState->GetScope().get() == this)
		{
			return ETaskScopeWaitResult::UnsupportedThread;
		}

		const auto Start = std::chrono::steady_clock::now();
		const double ClampedTimeout = std::max(0.0, TimeoutSeconds);
		auto IsQuiescent = [this]() {
			return State == ETaskScopeState::QuiescentDrain || State == ETaskScopeState::QuiescentCancel;
		};
		for (;;)
		{
			{
				std::unique_lock Lock(Mutex);
				if (IsQuiescent()) return ETaskScopeWaitResult::Quiescent;
				if (!bUnbounded)
				{
					const double Elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
					if (Elapsed >= ClampedTimeout) return ETaskScopeWaitResult::TimedOut;
					const double Slice = bWorkerHelping ? std::min(WorkerWaitSliceSeconds, ClampedTimeout - Elapsed) : ClampedTimeout - Elapsed;
					if (CV.wait_for(Lock, std::chrono::duration<double>(Slice), IsQuiescent)) return ETaskScopeWaitResult::Quiescent;
					if (!bWorkerHelping) return ETaskScopeWaitResult::TimedOut;
				}
				else if (!bWorkerHelping)
				{
					CV.wait(Lock, IsQuiescent);
					return ETaskScopeWaitResult::Quiescent;
				}
				else
				{
					CV.wait_for(Lock, std::chrono::duration<double>(WorkerWaitSliceSeconds), IsQuiescent);
					if (IsQuiescent()) return ETaskScopeWaitResult::Quiescent;
				}
			}
			if (bWorkerHelping) PinnedScheduler->TryExecuteOneQueuedTask();
		}
	}

	auto FTaskStateData::SetRetainedResultBytes(uint64 InRetainedResultBytes) -> void
	{
		uint64 PreviousRetainedResultBytes = 0;
		{
			std::lock_guard Lock(Mutex);
			PreviousRetainedResultBytes = RetainedResultBytes;
			RetainedResultBytes = InRetainedResultBytes;
		}
		if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
		{
			PinnedScheduler->OnRetainedResultBytesChanged(Attribution, PreviousRetainedResultBytes, InRetainedResultBytes);
		}
	}

	class FGameThreadDeferredWorkQueue final : public std::enable_shared_from_this<FGameThreadDeferredWorkQueue>
	{
	public:
		FGameThreadDeferredWorkQueue(FGameThreadDeferredWorkQueueConfig InConfig, uint64 InAdapterGeneration)
			: Config(std::move(InConfig))
			, AdapterGeneration(InAdapterGeneration)
		{
			Diagnostics.AdapterGeneration = InAdapterGeneration;
			Diagnostics.bInstalled = true;
			Diagnostics.bAccepting = true;
		}

		auto Enqueue(const std::shared_ptr<FTaskStateData>& State, Private::FMoveOnlyTaskFunction&& Function) -> bool
		{
			auto Entry = std::make_shared<FEntry>();
			Entry->State = State;
			Entry->Function = std::move(Function);
			Entry->PayloadBytes = State->GetEstimatedPayloadBytes();
			Entry->Priority = State->GetPriority();
			Entry->GenerationToken = State->GetGenerationToken();
			Entry->CoalescingKey = State->GetCoalescingKey();
			Entry->EnqueueTimeNanoseconds = MonotonicNanoseconds();
			std::shared_ptr<FTaskStateData> SupersededState;
			std::shared_ptr<FEntry> SupersededEntry;
			std::vector<std::shared_ptr<FEntry>> DetachedEntries;
			{
				std::lock_guard Lock(Mutex);
				PurgeTerminalEntriesLocked(DetachedEntries);
				const uint64 PayloadBytes = Entry->PayloadBytes;
				if (!bAccepting || PayloadBytes == 0 || PayloadBytes > Config.MaxPayloadBytesPerEntry)
				{
					++Diagnostics.RejectedCount;
					return false;
				}

				std::shared_ptr<FEntry> Replacement;
				if (State->GetCoalescingKey())
				{
					for (auto& Queue : Queues)
					{
						for (const std::shared_ptr<FEntry>& Candidate : Queue)
						{
							if (Candidate->bReserved && Candidate->CoalescingKey == State->GetCoalescingKey())
							{
								Replacement = Candidate;
								break;
							}
						}
						if (Replacement) break;
					}
				}

				const uint32 ReplacedEntries = Replacement ? 1u : 0u;
				const uint64 ReplacedBytes = Replacement ? Replacement->PayloadBytes : 0;
				if (ActiveEntryCount - ReplacedEntries + 1 > Config.MaxQueuedEntries
					|| QueuedPayloadBytes - ReplacedBytes + PayloadBytes > Config.MaxQueuedPayloadBytes)
				{
					++Diagnostics.RejectedCount;
					return false;
				}

				if (Replacement)
				{
					SupersededState = Replacement->State;
					SupersededEntry = Replacement;
					ReleaseReservationLocked(*Replacement);
					for (auto& Queue : Queues) std::erase(Queue, Replacement);
					++Diagnostics.SupersededCount;
				}

				Queues[static_cast<size_t>(Entry->Priority)].emplace_back(std::move(Entry));
				++ActiveEntryCount;
				QueuedPayloadBytes += PayloadBytes;
				++Diagnostics.AcceptedCount;
				RefreshDepthDiagnosticsLocked();
				Diagnostics.PeakQueueDepth = std::max(Diagnostics.PeakQueueDepth, ActiveEntryCount);
				Diagnostics.PeakQueuedPayloadBytes = std::max(Diagnostics.PeakQueuedPayloadBytes, QueuedPayloadBytes);
			}

			if (SupersededState)
			{
				SupersededState->RequestCancellation("GameThread deferred task was superseded.", ETaskTerminalReason::Superseded);
			}
			return true;
		}

		auto Pump(const FGameThreadDeferredPumpBudget& Budget) -> FGameThreadDeferredPumpResult
		{
			CheckGameThread();
			const auto PumpStart = std::chrono::steady_clock::now();
			FGameThreadDeferredPumpResult Result;
			{
				std::lock_guard Lock(Mutex);
				if (bPumping)
				{
					++Diagnostics.ReentrantPumpCount;
					return Result;
				}
				bPumping = true;
				GIsPumpingGameThreadDeferred = true;
			}

			while (Budget.bUnlimited || Result.ExecutedCallbacks < Budget.MaxCallbacks)
			{
				std::shared_ptr<FEntry> Entry;
				std::vector<std::shared_ptr<FEntry>> DetachedEntries;
				bool bExpiredGeneration = false;
				{
					std::lock_guard Lock(Mutex);
					PurgeTerminalEntriesLocked(DetachedEntries, &Result.TerminalEntriesSkipped);
					for (auto& Queue : Queues)
					{
						while (!Queue.empty() && !Queue.front()->bReserved) Queue.pop_front();
						if (!Queue.empty())
						{
							Entry = std::move(Queue.front());
							Queue.pop_front();
							break;
						}
					}
					if (!Entry) break;
					bExpiredGeneration = !Entry->GenerationToken.IsCurrent();
					ReleaseReservationLocked(*Entry);
					if (bExpiredGeneration) ++Diagnostics.ExpiredGenerationCount;
					RefreshDepthDiagnosticsLocked();
				}

				if (bExpiredGeneration)
				{
					Entry->State->RequestCancellation("GameThread deferred task generation expired.", ETaskTerminalReason::StaleGeneration);
					++Result.TerminalEntriesSkipped;
					continue;
				}

				const auto CallbackStart = std::chrono::steady_clock::now();
				if (std::shared_ptr<FTaskScheduler> Scheduler = Entry->State->PinScheduler())
				{
					Scheduler->ExecuteTask(Entry->State, std::move(Entry->Function), false);
				}
				else
				{
					Entry->State->RequestCancellation("Task scheduler was unavailable during GameThread dispatch.", ETaskTerminalReason::DispatchRejected);
				}
				const uint64 CallbackNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - CallbackStart).count());
				{
					std::lock_guard Lock(Mutex);
					if (Entry->State->GetState() == ETaskState::Failed) ++Diagnostics.CallbackFailureCount;
					if (CallbackNanoseconds >= static_cast<uint64>(Config.LongCallbackSeconds * 1'000'000'000.0))
					{
						++Diagnostics.LongCallbackCount;
						Diagnostics.LastLongCallbackTaskId = Entry->State->GetTaskId();
						Diagnostics.LastLongCallbackNanoseconds = CallbackNanoseconds;
					}
				}
				++Result.ExecutedCallbacks;

				if (!Budget.bUnlimited && std::chrono::duration<double>(std::chrono::steady_clock::now() - PumpStart).count() >= Budget.MaxSeconds)
				{
					break;
				}
			}

			Result.ElapsedNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - PumpStart).count());
			{
				std::lock_guard Lock(Mutex);
				bPumping = false;
				GIsPumpingGameThreadDeferred = false;
				++Diagnostics.PumpCount;
				Diagnostics.PumpedCallbackCount += Result.ExecutedCallbacks;
				Diagnostics.PumpTimeNanoseconds += Result.ElapsedNanoseconds;
			}
			return Result;
		}

		auto PumpFrame() -> FGameThreadDeferredPumpResult
		{
			return Pump({
				.MaxCallbacks = Config.FrameMaxCallbacks,
				.MaxSeconds = Config.FrameMaxSeconds,
			});
		}

		auto ProcessScope(
			const std::shared_ptr<FTaskScopeState>& Scope,
			bool bCancel,
			const FGameThreadDeferredPumpBudget& Budget
		) -> Private::FTaskScopeDeferredPumpResult
		{
			CheckGameThread();
			Private::FTaskScopeDeferredPumpResult Result;
			{
				std::lock_guard Lock(Mutex);
				if (bPumping)
				{
					++Diagnostics.ReentrantPumpCount;
					Result.bReentrant = true;
					return Result;
				}
				bPumping = true;
				GIsPumpingGameThreadDeferred = true;
			}

			const auto Start = std::chrono::steady_clock::now();
			for (;;)
			{
				if (!Budget.bUnlimited
					&& Result.ExecutedCallbacks + Result.CanceledCallbacks + Result.DestroyedCallables >= Budget.MaxCallbacks)
				{
					break;
				}

				std::shared_ptr<FEntry> Entry;
				bool bTerminalEntry = false;
				bool bExpiredGeneration = false;
				{
					std::lock_guard Lock(Mutex);
					for (auto& Queue : Queues)
					{
						const auto Iterator = std::ranges::find_if(Queue, [&](const std::shared_ptr<FEntry>& Candidate) {
							return Candidate->bReserved && Candidate->State->GetScope() == Scope;
						});
						if (Iterator == Queue.end()) continue;
						Entry = std::move(*Iterator);
						Queue.erase(Iterator);
						bTerminalEntry = IsTerminalState(Entry->State->GetState());
						bExpiredGeneration = !bTerminalEntry && !Entry->GenerationToken.IsCurrent();
						ReleaseReservationLocked(*Entry);
						if (bExpiredGeneration) ++Diagnostics.ExpiredGenerationCount;
						RefreshDepthDiagnosticsLocked();
						break;
					}
				}
				if (!Entry) break;

				if (bTerminalEntry)
				{
					Entry->Function = {};
					++Result.DestroyedCallables;
				}
				else if (bCancel || bExpiredGeneration)
				{
					Entry->State->RequestCancellation(
						bExpiredGeneration
							? "GameThread deferred task generation expired during selected drain."
							: "GameThread deferred task was canceled by its operation group.",
						bExpiredGeneration ? ETaskTerminalReason::StaleGeneration : ETaskTerminalReason::CancellationRequested);
					Entry->Function = {};
					++Result.CanceledCallbacks;
					++Result.DestroyedCallables;
				}
				else
				{
					if (std::shared_ptr<FTaskScheduler> Scheduler = Entry->State->PinScheduler())
					{
						Scheduler->ExecuteTask(Entry->State, std::move(Entry->Function), false);
					}
					else
					{
						Entry->State->RequestCancellation(
							"Task scheduler was unavailable during selected GameThread dispatch.",
							ETaskTerminalReason::DispatchRejected);
					}
					++Result.ExecutedCallbacks;
					++Result.DestroyedCallables;
				}

				if (!Budget.bUnlimited
					&& std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count() >= Budget.MaxSeconds)
				{
					break;
				}
			}

			{
				std::lock_guard Lock(Mutex);
				bPumping = false;
				GIsPumpingGameThreadDeferred = false;
			}
			return Result;
		}

		auto GetScopeSnapshot(
			const std::shared_ptr<FTaskScopeState>& Scope
		) -> Private::FTaskScopeDeferredWorkSnapshot
		{
			Private::FTaskScopeDeferredWorkSnapshot Result;
			std::lock_guard Lock(Mutex);
			for (const auto& Queue : Queues)
			{
				for (const std::shared_ptr<FEntry>& Entry : Queue)
				{
					if (!Entry->bReserved || Entry->State->GetScope() != Scope) continue;
					++Result.RetainedCallableCount;
					Result.RetainedCallableBytes += Entry->Function.GetStorageBytes();
				}
			}
			return Result;
		}

		auto CloseAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAccepting = false;
			Diagnostics.bAccepting = false;
		}

		auto CancelAll() -> void
		{
			std::vector<std::shared_ptr<FTaskStateData>> States;
			std::vector<std::shared_ptr<FEntry>> DetachedEntries;
			{
				std::lock_guard Lock(Mutex);
				bAccepting = false;
				Diagnostics.bAccepting = false;
				for (auto& Queue : Queues)
				{
					for (const std::shared_ptr<FEntry>& Entry : Queue)
					{
						if (!Entry->bReserved) continue;
						States.emplace_back(Entry->State);
						ReleaseReservationLocked(*Entry);
						DetachedEntries.emplace_back(Entry);
					}
					Queue.clear();
				}
				Diagnostics.CanceledCount += States.size();
				RefreshDepthDiagnosticsLocked();
			}
			for (const std::shared_ptr<FTaskStateData>& State : States)
			{
				State->RequestCancellation("GameThread deferred task was canceled during shutdown.", ETaskTerminalReason::ShutdownCanceled);
			}
		}

		auto GetDiagnostics() -> FGameThreadDeferredWorkQueueDiagnostics
		{
			std::vector<std::shared_ptr<FEntry>> DetachedEntries;
			std::lock_guard Lock(Mutex);
			PurgeTerminalEntriesLocked(DetachedEntries);
			RefreshDepthDiagnosticsLocked();
			uint64 OldestEnqueue = 0;
			for (const auto& Queue : Queues)
			{
				for (const std::shared_ptr<FEntry>& Entry : Queue)
				{
					if (Entry->bReserved && (OldestEnqueue == 0 || Entry->EnqueueTimeNanoseconds < OldestEnqueue))
					{
						OldestEnqueue = Entry->EnqueueTimeNanoseconds;
					}
				}
			}
			Diagnostics.OldestEntryAgeNanoseconds = OldestEnqueue == 0 ? 0 : MonotonicNanoseconds() - OldestEnqueue;
			return Diagnostics;
		}

		auto MarkUninstalled() -> FGameThreadDeferredWorkQueueDiagnostics
		{
			std::vector<std::shared_ptr<FEntry>> DetachedEntries;
			std::lock_guard Lock(Mutex);
			PurgeTerminalEntriesLocked(DetachedEntries);
			RefreshDepthDiagnosticsLocked();
			check(ActiveEntryCount == 0);
			bAccepting = false;
			Diagnostics.bAccepting = false;
			Diagnostics.bInstalled = false;
			return Diagnostics;
		}

	private:
		struct FEntry
		{
			std::shared_ptr<FTaskStateData> State;
			Private::FMoveOnlyTaskFunction Function;
			FTaskGenerationToken GenerationToken;
			std::optional<FTaskCoalescingKey> CoalescingKey;
			uint64 PayloadBytes = 0;
			uint64 EnqueueTimeNanoseconds = 0;
			ETaskPriority Priority = ETaskPriority::Normal;
			bool bReserved = true;
		};

		auto ReleaseReservationLocked(FEntry& Entry) -> void
		{
			if (!Entry.bReserved) return;
			check(ActiveEntryCount > 0);
			check(QueuedPayloadBytes >= Entry.PayloadBytes);
			--ActiveEntryCount;
			QueuedPayloadBytes -= Entry.PayloadBytes;
			Entry.bReserved = false;
		}

		auto PurgeTerminalEntriesLocked(std::vector<std::shared_ptr<FEntry>>& OutDetachedEntries, uint32* SkippedCount = nullptr) -> void
		{
			for (auto& Queue : Queues)
			{
				for (auto Iterator = Queue.begin(); Iterator != Queue.end();)
				{
					const std::shared_ptr<FEntry>& Entry = *Iterator;
					if (Entry->bReserved && !IsTerminalState(Entry->State->GetState()))
					{
						++Iterator;
						continue;
					}
					if (Entry->bReserved)
					{
						if (Entry->State->GetState() == ETaskState::Canceled) ++Diagnostics.CanceledCount;
						ReleaseReservationLocked(*Entry);
						if (SkippedCount) ++*SkippedCount;
					}
					OutDetachedEntries.emplace_back(std::move(*Iterator));
					Iterator = Queue.erase(Iterator);
				}
			}
		}

		auto RefreshDepthDiagnosticsLocked() -> void
		{
			Diagnostics.QueueDepth = ActiveEntryCount;
			Diagnostics.QueuedPayloadBytes = QueuedPayloadBytes;
			Diagnostics.PriorityDepths = {};
			for (size_t PriorityIndex = 0; PriorityIndex < Queues.size(); ++PriorityIndex)
			{
				for (const std::shared_ptr<FEntry>& Entry : Queues[PriorityIndex])
				{
					Diagnostics.PriorityDepths[PriorityIndex] += Entry->bReserved ? 1u : 0u;
				}
			}
		}

		FGameThreadDeferredWorkQueueConfig Config;
		uint64 AdapterGeneration = 0;
		std::mutex Mutex;
		std::array<std::deque<std::shared_ptr<FEntry>>, 3> Queues;
		FGameThreadDeferredWorkQueueDiagnostics Diagnostics;
		uint32 ActiveEntryCount = 0;
		uint64 QueuedPayloadBytes = 0;
		bool bAccepting = true;
		bool bPumping = false;
	};

	auto DispatchGameThreadDeferredTask(
		const std::shared_ptr<FTaskStateData>& State,
		Private::FMoveOnlyTaskFunction&& Function) -> bool
	{
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue && Queue->Enqueue(State, std::move(Function));
	}

	auto Private::IsExecutingTaskScope(const FTaskScopeToken& Scope) -> bool
	{
		const std::shared_ptr<FTaskScopeState>& State = FTaskScopeAccess::GetState(Scope);
		return State && GCurrentTaskState && GCurrentTaskState->GetScope() == State;
	}

	auto Private::ProcessGameThreadDeferredScope(
		const FTaskScopeToken& Scope,
		bool bCancel,
		const FGameThreadDeferredPumpBudget& Budget
	) -> FTaskScopeDeferredPumpResult
	{
		const std::shared_ptr<FTaskScopeState>& State = FTaskScopeAccess::GetState(Scope);
		if (!State) return {};
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue ? Queue->ProcessScope(State, bCancel, Budget) : FTaskScopeDeferredPumpResult{};
	}

	auto Private::GetGameThreadDeferredScopeSnapshot(
		const FTaskScopeToken& Scope
	) -> FTaskScopeDeferredWorkSnapshot
	{
		const std::shared_ptr<FTaskScopeState>& State = FTaskScopeAccess::GetState(Scope);
		if (!State) return {};
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue ? Queue->GetScopeSnapshot(State) : FTaskScopeDeferredWorkSnapshot{};
	}

	auto Private::WaitForTaskScopeWorkerCallables(
		const FTaskScopeToken& Scope,
		double TimeoutSeconds
	) -> bool
	{
		const std::shared_ptr<FTaskScopeState>& State = FTaskScopeAccess::GetState(Scope);
		if (!State) return true;
		const std::shared_ptr<FTaskScheduler> Scheduler = State->PinScheduler();
		return !Scheduler || Scheduler->WaitForScopeWorkerCallables(State->GetScopeId(), TimeoutSeconds);
	}

	auto Private::GetTaskScopeWorkerCallableCount(const FTaskScopeToken& Scope) -> uint32
	{
		const std::shared_ptr<FTaskScopeState>& State = FTaskScopeAccess::GetState(Scope);
		if (!State) return 0;
		const std::shared_ptr<FTaskScheduler> Scheduler = State->PinScheduler();
		return Scheduler ? Scheduler->GetScopeWorkerCallableCount(State->GetScopeId()) : 0;
	}

	auto FTaskStateData::PublishTerminalLocked(
		ETaskState TerminalState,
		std::string InDiagnostic,
		std::vector<std::shared_ptr<FTaskStateData>>& OutDependents,
		std::function<void(ETaskState)>& OutCompletionFunction,
		std::unique_ptr<Private::FMoveOnlyTaskFunction>& OutPendingFunction,
		ETaskTerminalReason InReason,
		uint64 InDirectBlockingTaskId
	) -> bool
	{
		if (IsTerminalState(State))
		{
			return false;
		}

		const bool bValidTransition =
			(State == ETaskState::Running && (TerminalState == ETaskState::Succeeded || TerminalState == ETaskState::Failed || TerminalState == ETaskState::Canceled))
			|| ((State == ETaskState::Waiting || State == ETaskState::Queued) && TerminalState == ETaskState::Canceled);
		check(bValidTransition);
		StateBeforeTerminal = State;
		bTerminalPublicationFinished = false;
		State = TerminalState;
		FinishTimeNanoseconds = MonotonicNanoseconds();
		Diagnostic = std::move(InDiagnostic);
		TerminalReason = InReason;
		DirectBlockingTaskId = InDirectBlockingTaskId;
		OutPendingFunction = std::move(PendingFunction);
		OutCompletionFunction = std::move(CompletionFunction);
		for (const std::weak_ptr<FTaskStateData>& Dependent : Dependents)
		{
			if (std::shared_ptr<FTaskStateData> PinnedDependent = Dependent.lock())
			{
				OutDependents.emplace_back(std::move(PinnedDependent));
			}
		}
		Dependents.clear();
		return true;
	}

	auto FTaskStateData::PublishTerminal(
		ETaskState TerminalState,
		std::string InDiagnostic,
		ETaskTerminalReason InReason,
		uint64 InDirectBlockingTaskId) -> bool
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		{
			std::lock_guard Lock(Mutex);
			if (!PublishTerminalLocked(TerminalState, std::move(InDiagnostic), DependentsToNotify, Function, PendingFunction, InReason, InDirectBlockingTaskId))
			{
				return false;
			}
		}
		InvokeTaskTerminalPublicationTestHook(TaskId);
		if (Function) Function(TerminalState);
		Function = {};
		PendingFunction.reset();
		FinishTerminalPublication(TerminalState, std::move(DependentsToNotify));
		return true;
	}

	auto FTaskStateData::FinishTerminalPublication(ETaskState TerminalState, std::vector<std::shared_ptr<FTaskStateData>>&& Dependents) -> void
	{
		if (SharedCancellationState)
		{
			SharedCancellationState->UnregisterTask(TaskId);
		}
		{
			std::lock_guard Lock(Mutex);
			require(!bTerminalLifetimeCharged);
			LifetimeAccounting->RetainedTerminalTaskCount.fetch_add(1, std::memory_order::acq_rel);
			bTerminalLifetimeCharged = true;
			if (bHasResultStorage)
			{
				LifetimeAccounting->RetainedTerminalResultCount.fetch_add(1, std::memory_order::acq_rel);
				bTerminalResultLifetimeCharged = true;
				if (Scope) Scope->ChargeRetainedResult();
			}
			bTerminalPublicationFinished = true;
		}
		CV.notify_all();
		for (const std::shared_ptr<FTaskStateData>& Dependent : Dependents)
		{
			Dependent->OnPrerequisiteTerminal(TerminalState, TaskId);
		}
		if (Scope)
		{
			Scope->ReleaseTask(TaskId, TerminalState);
		}
		if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
		{
			PinnedScheduler->OnTaskTerminal(shared_from_this());
		}
	}

	auto FTaskStateData::OnPrerequisiteTerminal(ETaskState PrerequisiteState, uint64 PrerequisiteTaskId) -> void
	{
		if (bAggregatePrerequisites)
		{
			bool bShouldQueue = false;
			bool bShouldCancel = false;
			ETaskState BlockingState = ETaskState::Succeeded;
			uint64 BlockingTaskId = 0;
			{
				std::lock_guard Lock(Mutex);
				if (State != ETaskState::Waiting)
				{
					return;
				}
				check(RemainingPrerequisites > 0);
				--RemainingPrerequisites;
				if (DependencyKind == ETaskDependencyKind::Success && PrerequisiteState != ETaskState::Succeeded)
				{
					const bool bPreferPrerequisite = BlockingPrerequisiteTaskId == 0
						|| (PrerequisiteState == ETaskState::Failed && BlockingPrerequisiteState != ETaskState::Failed)
						|| (PrerequisiteState == BlockingPrerequisiteState && PrerequisiteTaskId < BlockingPrerequisiteTaskId);
					if (bPreferPrerequisite)
					{
						BlockingPrerequisiteTaskId = PrerequisiteTaskId;
						BlockingPrerequisiteState = PrerequisiteState;
					}
				}
				if (RemainingPrerequisites == 0)
				{
					BlockingState = BlockingPrerequisiteState;
					BlockingTaskId = BlockingPrerequisiteTaskId;
					bShouldCancel = DependencyKind == ETaskDependencyKind::Success && BlockingTaskId != 0;
					if (!bShouldCancel)
					{
						State = ETaskState::Queued;
						bShouldQueue = true;
					}
				}
			}

			if (bShouldCancel)
			{
				RequestCancellation(
					"Task was canceled because prerequisite " + std::to_string(BlockingTaskId)
						+ (BlockingState == ETaskState::Failed ? " failed." : " was canceled."),
					BlockingState == ETaskState::Failed ? ETaskTerminalReason::DependencyFailed : ETaskTerminalReason::DependencyCanceled,
					BlockingTaskId
				);
			}
			else if (bShouldQueue)
			{
				if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
				{
					PinnedScheduler->OnTaskQueued(Attribution);
					PinnedScheduler->QueueTask(shared_from_this());
				}
				else
				{
					RequestCancellation("Task scheduler was unavailable when prerequisites completed.", ETaskTerminalReason::DispatchRejected);
				}
			}
			return;
		}

		if (PrerequisiteState != ETaskState::Succeeded)
		{
			RequestCancellation(
				"Task was canceled because prerequisite " + std::to_string(PrerequisiteTaskId)
					+ (PrerequisiteState == ETaskState::Failed ? " failed." : " was canceled."),
				PrerequisiteState == ETaskState::Failed ? ETaskTerminalReason::DependencyFailed : ETaskTerminalReason::DependencyCanceled,
				PrerequisiteTaskId
			);
			return;
		}

		bool bShouldQueue = false;
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Waiting)
			{
				return;
			}
			check(RemainingPrerequisites > 0);
			--RemainingPrerequisites;
			if (RemainingPrerequisites == 0)
			{
				State = ETaskState::Queued;
				bShouldQueue = true;
			}
		}

		if (bShouldQueue)
		{
			if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
			{
				PinnedScheduler->OnTaskQueued(Attribution);
				PinnedScheduler->QueueTask(shared_from_this());
			}
			else
			{
				RequestCancellation("Task scheduler was unavailable when prerequisites completed.", ETaskTerminalReason::DispatchRejected);
			}
		}
	}

	auto FTaskStateData::RequestCancellation(std::string InDiagnostic, ETaskTerminalReason InReason, uint64 InDirectBlockingTaskId) -> bool
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		bool bPublishedTerminal = false;
		{
			std::lock_guard Lock(Mutex);
			if (IsTerminalState(State))
			{
				return false;
			}

			bCancellationRequested = true;
			if (CancellationDiagnostic.empty())
			{
				CancellationDiagnostic = std::move(InDiagnostic);
				CancellationReason = InReason;
				CancellationDirectBlockingTaskId = InDirectBlockingTaskId;
			}
			if (State == ETaskState::Waiting || State == ETaskState::Queued)
			{
				bPublishedTerminal = PublishTerminalLocked(ETaskState::Canceled, CancellationDiagnostic, DependentsToNotify, Function, PendingFunction, InReason, InDirectBlockingTaskId);
			}
		}

		if (bPublishedTerminal)
		{
			InvokeTaskTerminalPublicationTestHook(TaskId);
			if (Function) Function(ETaskState::Canceled);
			FinishTerminalPublication(ETaskState::Canceled, std::move(DependentsToNotify));
		}
		return true;
	}

	auto FTaskStateData::MarkSucceeded() -> void
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		ETaskState TerminalState = ETaskState::Succeeded;
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Running)
			{
				return;
			}
			if (bCancellationRequested || (SharedCancellationState && SharedCancellationState->IsCancellationRequested()))
			{
				TerminalState = ETaskState::Canceled;
			}
			PublishTerminalLocked(
				TerminalState,
				TerminalState == ETaskState::Canceled ? (CancellationDiagnostic.empty() ? "Task returned after cancellation was requested." : CancellationDiagnostic) : std::string{},
				DependentsToNotify,
				Function,
				PendingFunction,
				TerminalState == ETaskState::Canceled ? CancellationReason : ETaskTerminalReason::None,
				TerminalState == ETaskState::Canceled ? CancellationDirectBlockingTaskId : 0
			);
		}
		InvokeTaskTerminalPublicationTestHook(TaskId);
		if (Function) Function(TerminalState);
		FinishTerminalPublication(TerminalState, std::move(DependentsToNotify));
	}

	auto FTaskStateData::MarkFailed(std::string InDiagnostic) -> void
	{
		PublishTerminal(ETaskState::Failed, std::move(InDiagnostic), ETaskTerminalReason::CallbackFailure);
	}

	auto FTaskStateData::MarkCanceled(std::string InDiagnostic) -> void
	{
		RequestCancellation(std::move(InDiagnostic));
	}

	auto FTaskCancellationState::RegisterTask(const std::shared_ptr<FTaskStateData>& Task) -> void
	{
		bool bAlreadyCanceled = false;
		{
			std::lock_guard Lock(Mutex);
			bAlreadyCanceled = bCancellationRequested;
			if (!bAlreadyCanceled)
			{
				Tasks.emplace(Task->GetTaskId(), Task);
			}
		}
		if (bAlreadyCanceled)
		{
			Task->RequestCancellation("Task cancellation was requested by its shared source.");
		}
	}

	auto FTaskCancellationState::UnregisterTask(uint64 TaskId) -> void
	{
		std::lock_guard Lock(Mutex);
		Tasks.erase(TaskId);
	}

	auto FTaskCancellationState::RequestCancellation() -> void
	{
		std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
		{
			std::lock_guard Lock(Mutex);
			if (bCancellationRequested)
			{
				return;
			}
			bCancellationRequested = true;
			TasksToCancel.reserve(Tasks.size());
			for (const auto& RegisteredTask : Tasks)
			{
				if (std::shared_ptr<FTaskStateData> PinnedTask = RegisteredTask.second.lock())
				{
					TasksToCancel.emplace_back(std::move(PinnedTask));
				}
			}
			Tasks.clear();
		}
		for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
		{
			Task->RequestCancellation("Task cancellation was requested by its shared source.");
		}
	}

	FTaskCancellationToken::FTaskCancellationToken() = default;

	FTaskCancellationToken::FTaskCancellationToken(
		std::shared_ptr<FTaskCancellationState> InSharedState,
		std::weak_ptr<FTaskStateData> InTaskState
	)
		: SharedState(std::move(InSharedState))
		, TaskState(std::move(InTaskState))
	{
	}

	auto FTaskCancellationToken::IsCancellationRequested() const -> bool
	{
		if (std::shared_ptr<FTaskStateData> PinnedTask = TaskState.lock(); PinnedTask && PinnedTask->IsCancellationRequested())
		{
			return true;
		}
		return SharedState && SharedState->IsCancellationRequested();
	}

	FTaskCancellationSource::FTaskCancellationSource()
		: State(std::make_shared<FTaskCancellationState>())
	{
	}

	auto FTaskCancellationSource::GetToken() const -> FTaskCancellationToken
	{
		return FTaskCancellationToken(State, {});
	}

	auto FTaskCancellationSource::IsCancellationRequested() const -> bool
	{
		return State && State->IsCancellationRequested();
	}

	auto FTaskCancellationSource::RequestCancellation() -> void
	{
		if (State)
		{
			State->RequestCancellation();
		}
	}

	FTaskGenerationToken::FTaskGenerationToken(
		std::shared_ptr<FTaskGenerationState> InState,
		uint64 InGeneration)
		: State(std::move(InState))
		, Generation(InGeneration)
	{
	}

	auto FTaskGenerationToken::IsCurrent() const -> bool
	{
		return !State || State->Generation.load(std::memory_order::acquire) == Generation;
	}

	auto FTaskGenerationToken::IsConstrained() const -> bool
	{
		return State != nullptr;
	}

	FTaskGenerationSource::FTaskGenerationSource()
		: State(std::make_shared<FTaskGenerationState>())
	{
	}

	auto FTaskGenerationSource::Capture() const -> FTaskGenerationToken
	{
		return FTaskGenerationToken(State, GetGeneration());
	}

	auto FTaskGenerationSource::Advance() -> uint64
	{
		return State ? State->Generation.fetch_add(1, std::memory_order::acq_rel) + 1 : 0;
	}

	auto FTaskGenerationSource::GetGeneration() const -> uint64
	{
		return State ? State->Generation.load(std::memory_order::acquire) : 0;
	}

	FParallelForCancellationToken::FParallelForCancellationToken(FTaskCancellationToken InGroupToken, FTaskCancellationToken InExternalToken)
		: GroupToken(std::move(InGroupToken))
		, ExternalToken(std::move(InExternalToken))
	{
	}

	auto FParallelForCancellationToken::IsCancellationRequested() const -> bool
	{
		return GroupToken.IsCancellationRequested() || ExternalToken.IsCancellationRequested();
	}

	FTaskHandle::FTaskHandle() = default;

	FTaskHandle::FTaskHandle(std::shared_ptr<FTaskStateData> InState)
		: State(std::move(InState))
	{
	}

	auto FTaskHandle::IsValid() const -> bool
	{
		return State != nullptr;
	}

	auto FTaskHandle::IsComplete() const -> bool
	{
		return State && IsTerminalState(State->GetState());
	}

	auto FTaskHandle::GetState() const -> ETaskState
	{
		return State ? State->GetState() : ETaskState::Invalid;
	}

	auto FTaskHandle::GetDebugName() const -> const char*
	{
		return State ? State->GetDebugName() : "";
	}

	auto FTaskHandle::GetTaskId() const -> uint64
	{
		return State ? State->GetTaskId() : 0;
	}

	auto FTaskHandle::GetDiagnostic() const -> std::string
	{
		return State ? State->GetDiagnostic() : std::string{};
	}

	auto FTaskHandle::GetDiagnostics() const -> FTaskDiagnostics
	{
		return State ? State->GetDiagnostics() : FTaskDiagnostics{};
	}

	auto InitializeTaskScheduler(uint32 InNumThreads) -> bool
	{
		return InitializeTaskScheduler(FTaskSchedulerConfig{.NumWorkerThreads = InNumThreads});
	}

	auto CreateTaskScope() -> FTaskScope
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running || !GTaskScheduler)
		{
			return {};
		}
		return FTaskScope(GTaskScheduler->CreateScope());
	}

	auto InitializeTaskScheduler(const FTaskSchedulerConfig& Config) -> bool
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running)
		{
			DURIN_WARN("Task scheduler initialization ignored because it is already running.");
			return true;
		}
		if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::ShuttingDown)
		{
			DURIN_ERROR("Task scheduler initialization rejected while shutdown is in progress.");
			return false;
		}
		if (Config.MaxNonterminalTasks == 0 || Config.MaxNonterminalTasks > std::numeric_limits<uint32>::max())
		{
			DURIN_ERROR("Task scheduler initialization rejected because capacity must be between 1 and uint32 max.");
			return false;
		}

		GTaskScheduler = FTaskScheduler::Create(Config);
		if (!GTaskScheduler)
		{
			DURIN_ERROR("Task scheduler initialization failed.");
			return false;
		}

		GLastTaskSchedulerDiagnostics = {};
		GLastTaskSchedulerLifetimeAccounting.reset();
		GTaskSchedulerLifetime = ETaskSchedulerLifetime::Running;
		DURIN_DEBUG(
			"Task scheduler initialized. (workers: {}, capacity: {})",
			Config.NumWorkerThreads > 0 ? Config.NumWorkerThreads : GetDefaultThreadPoolThreadCount(),
			Config.MaxNonterminalTasks);
		return true;
	}

	auto InitializeGameThreadDeferredExecutor(const FGameThreadDeferredWorkQueueConfig& Config) -> bool
	{
		CheckGameThread();
		if (Config.MaxQueuedEntries == 0 || Config.MaxQueuedPayloadBytes == 0
			|| Config.MaxPayloadBytesPerEntry == 0 || Config.FrameMaxCallbacks == 0
			|| Config.FrameMaxSeconds <= 0.0 || Config.LongCallbackSeconds <= 0.0)
		{
			return false;
		}
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running) return false;
		}
		std::lock_guard Lock(GGameThreadDeferredQueueMutex);
		if (GGameThreadDeferredQueue) return true;
		const uint64 AdapterGeneration = GNextGameThreadDeferredAdapterGeneration.fetch_add(1, std::memory_order::acq_rel);
		GGameThreadDeferredQueue = std::make_shared<FGameThreadDeferredWorkQueue>(Config, AdapterGeneration);
		GLastGameThreadDeferredQueueDiagnostics = {};
		return true;
	}

	auto PumpGameThreadDeferredWork() -> FGameThreadDeferredPumpResult
	{
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue ? Queue->PumpFrame() : FGameThreadDeferredPumpResult{};
	}

	auto PumpGameThreadDeferredWork(const FGameThreadDeferredPumpBudget& Budget) -> FGameThreadDeferredPumpResult
	{
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue ? Queue->Pump(Budget) : FGameThreadDeferredPumpResult{};
	}

	auto GetGameThreadDeferredWorkQueueDiagnostics() -> FGameThreadDeferredWorkQueueDiagnostics
	{
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
			if (!Queue) return GLastGameThreadDeferredQueueDiagnostics;
		}
		return Queue->GetDiagnostics();
	}

	auto ShutdownTaskSystem(ETaskShutdownMode Mode) -> void
	{
		CheckGameThread();
		if (GIsPumpingGameThreadDeferred)
		{
			DURIN_WARN("Task-system shutdown request from a deferred callback was rejected.");
			return;
		}
		std::shared_ptr<FTaskScheduler> SchedulerToDestroy;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Stopped) return;
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::ShuttingDown)
			{
				DURIN_WARN("Recursive task-system shutdown request was rejected.");
				return;
			}
			GTaskSchedulerLifetime = ETaskSchedulerLifetime::ShuttingDown;
			SchedulerToDestroy = GTaskScheduler;
			SchedulerToDestroy->CloseAdmission();
		}
		SchedulerToDestroy->CloseLiveScopes(
			Mode == ETaskShutdownMode::Cancel ? ETaskScopeCloseMode::Cancel : ETaskScopeCloseMode::Drain);

		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}

		if (Mode == ETaskShutdownMode::Cancel)
		{
			if (Queue) Queue->CancelAll();
			SchedulerToDestroy->Shutdown(false);
		}
		else
		{
			const FGameThreadDeferredPumpBudget ShutdownBudget{.bUnlimited = true};
			const auto ShutdownWaitStart = std::chrono::steady_clock::now();
			bool bRecordedLongWait = false;
			while (SchedulerToDestroy->GetActiveTaskCount() > 0)
			{
				const FGameThreadDeferredPumpResult PumpResult = Queue
					? Queue->Pump(ShutdownBudget)
					: FGameThreadDeferredPumpResult{};
				if (PumpResult.ExecutedCallbacks == 0 && PumpResult.TerminalEntriesSkipped == 0)
				{
					SchedulerToDestroy->WaitForGraphProgress(0.001);
				}
				if (!bRecordedLongWait && std::chrono::duration<double>(std::chrono::steady_clock::now() - ShutdownWaitStart).count() >= LongWaitThresholdSeconds)
				{
					bRecordedLongWait = true;
					SchedulerToDestroy->RecordShutdownLongWait();
					DURIN_WARN("Task-system shutdown is waiting for accepted cross-executor work.");
				}
			}
			SchedulerToDestroy->Shutdown(true);
		}

		if (Queue)
		{
			Queue->CloseAdmission();
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			if (GGameThreadDeferredQueue == Queue)
			{
				GLastGameThreadDeferredQueueDiagnostics = Queue->MarkUninstalled();
				GGameThreadDeferredQueue.reset();
			}
		}

		FTaskSchedulerDiagnostics ShutdownDiagnostics = SchedulerToDestroy->GetDiagnostics(false);
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			check(GTaskScheduler == SchedulerToDestroy);
			GLastTaskSchedulerDiagnostics = std::make_shared<FTaskSchedulerDiagnostics>(ShutdownDiagnostics);
			GLastTaskSchedulerLifetimeAccounting = SchedulerToDestroy->GetLifetimeAccounting();
			GTaskScheduler.reset();
			GTaskSchedulerLifetime = ETaskSchedulerLifetime::Stopped;
			++GCompletedTaskSchedulerShutdowns;
		}
		GTaskSchedulerCV.notify_all();
	}

	auto ShutdownTaskScheduler(bool bWaitForQueuedWork) -> void
	{
		bool bUseCoordinator = false;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			bUseCoordinator = GGameThreadDeferredQueue && IsInGameThread();
		}
		if (bUseCoordinator)
		{
			ShutdownTaskSystem(bWaitForQueuedWork ? ETaskShutdownMode::Drain : ETaskShutdownMode::Cancel);
			return;
		}

		std::shared_ptr<FTaskScheduler> SchedulerToDestroy;
		{
			std::unique_lock Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::ShuttingDown)
			{
				const uint64 ObservedCompletedShutdowns = GCompletedTaskSchedulerShutdowns;
				GTaskSchedulerCV.wait(Lock, [ObservedCompletedShutdowns]() {
					return GCompletedTaskSchedulerShutdowns > ObservedCompletedShutdowns;
				});
				return;
			}
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Stopped)
			{
				return;
			}

			GTaskSchedulerLifetime = ETaskSchedulerLifetime::ShuttingDown;
			SchedulerToDestroy = GTaskScheduler;
			SchedulerToDestroy->CloseAdmission();
		}
		SchedulerToDestroy->CloseLiveScopes(
			bWaitForQueuedWork ? ETaskScopeCloseMode::Drain : ETaskScopeCloseMode::Cancel);

		SchedulerToDestroy->Shutdown(bWaitForQueuedWork);
		FTaskSchedulerDiagnostics ShutdownDiagnostics = SchedulerToDestroy->GetDiagnostics(false);
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			check(GTaskScheduler == SchedulerToDestroy);
			GLastTaskSchedulerDiagnostics = std::make_shared<FTaskSchedulerDiagnostics>(ShutdownDiagnostics);
			GLastTaskSchedulerLifetimeAccounting = SchedulerToDestroy->GetLifetimeAccounting();
			GTaskScheduler.reset();
			GTaskSchedulerLifetime = ETaskSchedulerLifetime::Stopped;
			++GCompletedTaskSchedulerShutdowns;
		}
		GTaskSchedulerCV.notify_all();
		DURIN_DEBUG(
			"Task scheduler shut down. (drained: {}, completed: {}, failed: {}, canceled: {}, rejected: {}, retained terminal handles: {})",
			bWaitForQueuedWork,
			ShutdownDiagnostics.CompletedTaskCount,
			ShutdownDiagnostics.FailedTaskCount,
			ShutdownDiagnostics.CanceledTaskCount,
			ShutdownDiagnostics.RejectedTaskCount,
			ShutdownDiagnostics.RetainedTerminalHandleCount
		);
	}

	auto IsTaskSchedulerRunning() -> bool
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		return GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running;
	}

	auto GetTaskSchedulerDiagnostics() -> FTaskSchedulerDiagnostics
	{
		std::shared_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<const FTaskSchedulerDiagnostics> LastDiagnostics;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting;
		bool bRunning = false;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			Scheduler = GTaskScheduler;
			bRunning = GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running;
			LastDiagnostics = GLastTaskSchedulerDiagnostics;
			LifetimeAccounting = GLastTaskSchedulerLifetimeAccounting;
		}
		if (Scheduler)
		{
			return Scheduler->GetDiagnostics(bRunning);
		}
		FTaskSchedulerDiagnostics Snapshot = LastDiagnostics ? *LastDiagnostics : FTaskSchedulerDiagnostics{};
		if (LifetimeAccounting)
		{
			Snapshot.RetainedTerminalHandleCount = LifetimeAccounting->RetainedTerminalTaskCount.load(std::memory_order::acquire);
			Snapshot.RetainedTerminalResultCount = LifetimeAccounting->RetainedTerminalResultCount.load(std::memory_order::acquire);
		}
		Snapshot.AttributionRegistrationOverflowCount = GTaskAttributionRegistry.GetOverflowCount();
		std::vector<FTaskOwnerCategoryDiagnostics> RegisteredPairs = GTaskAttributionRegistry.Snapshot();
		for (const FTaskOwnerCategoryDiagnostics& Existing : Snapshot.OwnerCategoryDiagnostics)
		{
			if (Existing.CategoryId < RegisteredPairs.size()
				&& RegisteredPairs[Existing.CategoryId].OwnerId == Existing.OwnerId)
			{
				RegisteredPairs[Existing.CategoryId] = Existing;
			}
		}
		Snapshot.OwnerCategoryDiagnostics = std::move(RegisteredPairs);
		return Snapshot;
	}

	auto PublishTaskSchedulerProfilerPlots() -> void
	{
		std::shared_ptr<FTaskScheduler> Scheduler;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			Scheduler = GTaskScheduler;
		}
		if (Scheduler) Scheduler->PublishProfilerPlots();
	}

	auto LaunchTask(const char* Name, FTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle
	{
		if (!Function)
		{
			FTaskAttribution Attribution = Options.Attribution;
			if (Private::FTaskAttributionAccess::IsDefault(Attribution) && GCurrentTaskState && GCurrentTaskScheduler)
			{
				Attribution = GCurrentTaskState->GetAttribution();
			}
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskScheduler)
			{
				GTaskScheduler->RecordRejectedTask(Attribution, 0);
			}
			DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
			return {};
		}
		return LaunchCancelableTask(
			Name,
			[Function = std::move(Function)](const FTaskCancellationToken&) mutable {
				Function();
			},
			Options
		);
	}

	auto LaunchCancelableTask(const char* Name, FCancelableTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle
	{
		if (!Function)
		{
			return Private::LaunchCancelableTaskWithCompletion(Name, {}, {}, Options);
		}
		return Private::LaunchCancelableTaskWithCompletion(Name, std::move(Function), {}, Options);
	}

	namespace Private
	{
		auto SetTaskTerminalPublicationTestHook(std::function<void(uint64)>&& Hook) -> void
		{
			std::lock_guard Lock(GTaskTerminalPublicationTestHookMutex);
			GTaskTerminalPublicationTestHook = std::move(Hook);
			GTaskTerminalPublicationTestHookEnabled.store(static_cast<bool>(GTaskTerminalPublicationTestHook), std::memory_order::release);
		}

		auto SetTaskSchedulerSnapshotTestHook(std::function<void()>&& Hook) -> void
		{
			std::lock_guard Lock(GTaskSchedulerSnapshotTestHookMutex);
			GTaskSchedulerSnapshotTestHook = std::move(Hook);
			GTaskSchedulerSnapshotTestHookEnabled.store(static_cast<bool>(GTaskSchedulerSnapshotTestHook), std::memory_order::release);
		}

		auto LaunchCancelableTaskWithCompletion(
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			uint64 EstimatedResultBytes) -> FTaskHandle
		{
			FTaskLaunchOptions ResolvedOptions = Options;
			if (FTaskAttributionAccess::IsDefault(ResolvedOptions.Attribution) && GCurrentTaskState && GCurrentTaskScheduler)
			{
				ResolvedOptions.Attribution = GCurrentTaskState->GetAttribution();
			}
			if (!Function)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask(ResolvedOptions.Attribution, 0);
				}
				DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
				return {};
			}
			auto FunctionOwner = std::make_unique<FMoveOnlyTaskFunction>(std::move(Function));

			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask(ResolvedOptions.Attribution, FunctionOwner->GetStorageBytes());
				}
				DURIN_WARN("Task launch failed because the task scheduler is not running. (task: {})", Name ? Name : "");
				return {};
			}

			std::shared_ptr<FTaskStateData> State = GTaskScheduler->Submit(
				Name,
				FunctionOwner,
				std::move(CompletionFunction),
				ResolvedOptions,
				ETaskDependencyKind::Success,
				false,
				ETaskTarget::AnyWorker,
				ETaskPriority::Normal,
				0,
				EstimatedResultBytes
			);
			if (!State)
			{
				DURIN_WARN("Task launch failed because its prerequisites were invalid or scheduler admission was closed. (task: {})", Name ? Name : "");
				return {};
			}
			return FTaskHandle(std::move(State));
		}

		auto LaunchContinuationTask(
			const FTaskHandle& Predecessor,
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskContinuationOptions& Options,
			ETaskDependencyKind DependencyKind,
			uint64 EstimatedResultBytes) -> FTaskHandle
		{
			FTaskAttribution ResolvedAttribution = FTaskAttributionAccess::IsDefault(Options.Attribution) && Predecessor.State
				? Predecessor.State->GetAttribution()
				: Options.Attribution;
			const FTaskScopeToken ResolvedScope = Predecessor.State ? Predecessor.State->GetScopeToken() : FTaskScopeToken{};
			if (!(Options.Scope == FTaskScopeToken{}) && !(Options.Scope == ResolvedScope))
			{
				if (const std::shared_ptr<FTaskScopeState>& RejectedScope = FTaskScopeAccess::GetState(Options.Scope))
				{
					RejectedScope->RecordRejected();
				}
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask(ResolvedAttribution, 0);
					GTaskScheduler->RecordScopeRejectedTask();
				}
				return {};
			}
			if (!Function)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(ResolvedAttribution, 0);
				return {};
			}
			auto FunctionOwner = std::make_unique<FMoveOnlyTaskFunction>(std::move(Function));
			std::vector<FTaskHandle> Prerequisites;
			Prerequisites.reserve(Options.Prerequisites.size() + 1);
			auto AppendUnique = [&Prerequisites](const FTaskHandle& Candidate) {
				if (std::ranges::none_of(Prerequisites, [&Candidate](const FTaskHandle& Existing) {
					return Existing.GetTaskId() == Candidate.GetTaskId();
				}))
				{
					Prerequisites.emplace_back(Candidate);
				}
			};
			AppendUnique(Predecessor);
			for (const FTaskHandle& Prerequisite : Options.Prerequisites) AppendUnique(Prerequisite);

			FTaskLaunchOptions LaunchOptions;
			LaunchOptions.Prerequisites = Prerequisites;
			LaunchOptions.CancellationToken = Options.CancellationToken;
			LaunchOptions.Attribution = ResolvedAttribution;
			LaunchOptions.Scope = ResolvedScope;

			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(ResolvedAttribution, FunctionOwner->GetStorageBytes());
				return {};
			}
			std::shared_ptr<FTaskStateData> State = GTaskScheduler->Submit(
				Name,
				FunctionOwner,
				std::move(CompletionFunction),
				LaunchOptions,
				DependencyKind,
				true,
				Options.Target,
				Options.Priority,
				Options.EstimatedPayloadBytes,
				EstimatedResultBytes,
				Options.GenerationToken,
				Options.CoalescingKey,
				true
			);
			return State ? FTaskHandle(std::move(State)) : FTaskHandle{};
		}

		auto MakeTaskRetainedResultBytesSetter(const FTaskHandle& Task) -> std::function<void(uint64)>
		{
			std::weak_ptr<FTaskStateData> WeakState = Task.State;
			return [WeakState = std::move(WeakState)](uint64 RetainedResultBytes) {
				if (std::shared_ptr<FTaskStateData> State = WeakState.lock())
				{
					State->SetRetainedResultBytes(RetainedResultBytes);
				}
			};
		}

		auto RecordDuplicateUniqueConsumerClaim() -> void
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskScheduler) GTaskScheduler->RecordDuplicateUniqueConsumerClaim();
			DURIN_WARN("Unique task consumer registration failed because the result already has a consuming continuation.");
		}

		auto RecordRejectedUniqueTask(const char* Name, const char* Diagnostic, FTaskAttribution Attribution) -> void
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(Attribution, 0);
			DURIN_WARN("{} (task: {})", Diagnostic ? Diagnostic : "Unique task registration failed.", Name ? Name : "");
		}
	} // namespace Private

	auto CancelTask(const FTaskHandle& Task) -> bool
	{
		return Task.State && Task.State->RequestCancellation("Task cancellation was requested through its handle.");
	}

	auto WaitTask(const FTaskHandle& Task) -> ETaskState
	{
		if (!Task.State)
		{
			return ETaskState::Invalid;
		}

		ETaskState State = Task.State->GetState();
		if (IsTerminalState(State))
		{
			return State;
		}

		if (GCurrentTaskState == Task.State.get())
		{
			DURIN_WARN("Task wait rejected because a task cannot wait for itself. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		if (GCurrentTaskState && Task.State->DependsOn(GCurrentTaskState))
		{
			DURIN_WARN("Task wait rejected because the target depends on the current task. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		if (IsInRenderingThread())
		{
			DURIN_WARN("Task wait rejected on the rendering thread. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}
		if (Task.State->GetTarget() == ETaskTarget::GameThreadDeferred && IsInGameThread())
		{
			DURIN_WARN("Task wait rejected because GameThread cannot block on deferred GameThread work. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		std::shared_ptr<FTaskScheduler> Scheduler = Task.State->PinScheduler();
		const auto WaitStartTime = std::chrono::steady_clock::now();
		bool bReportedLongWait = false;
		auto ReportLongWait = [&](bool bThresholdElapsed = false) {
			if (bReportedLongWait || (!bThresholdElapsed && std::chrono::duration<double>(std::chrono::steady_clock::now() - WaitStartTime).count() < LongWaitThresholdSeconds))
			{
				return;
			}
			bReportedLongWait = true;
			if (Scheduler)
			{
				const char* WaiterName = GCurrentTaskState
					? GCurrentTaskState->GetDebugName()
					: (GetCurrentThread() ? GetCurrentThread()->GetThreadName() : "ExternalThread");
				const FTaskDiagnostics TargetDiagnostics = Task.State->GetDiagnostics();
				const uint64 ElapsedNanoseconds = static_cast<uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - WaitStartTime).count()
				);
				Scheduler->RecordLongWait(
					WaiterName,
					TargetDiagnostics,
					ElapsedNanoseconds
				);
				DURIN_WARN(
					"Long task wait detected. (waiter: {}, target: {}, target id: {}, state: {}, elapsed milliseconds: {})",
					WaiterName,
					TargetDiagnostics.DebugName,
					TargetDiagnostics.TaskId,
					static_cast<uint32>(TargetDiagnostics.State),
					ElapsedNanoseconds / 1'000'000
				);
			}
		};
		if (GCurrentTaskState && Scheduler && GCurrentTaskScheduler == Scheduler.get())
		{
			while (!IsTerminalState(State))
			{
				if (Scheduler->TryExecuteOneQueuedTask())
				{
					State = Task.State->GetState();
					ReportLongWait();
					continue;
				}
				State = Task.State->WaitFor(WorkerWaitSliceSeconds);
				ReportLongWait();
			}
			return State;
		}

		State = Task.State->WaitFor(LongWaitThresholdSeconds);
		if (!IsTerminalState(State))
		{
			ReportLongWait(true);
			State = Task.State->Wait();
		}
		return State;
	}

	auto WaitAll(std::span<const FTaskHandle> Tasks) -> std::vector<ETaskState>
	{
		std::vector<ETaskState> Outcomes;
		Outcomes.reserve(Tasks.size());
		for (const FTaskHandle& Task : Tasks)
		{
			Outcomes.push_back(WaitTask(Task));
		}
		return Outcomes;
	}

	auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		if (!Function)
		{
			return {ETaskState::Invalid, "ParallelFor function is empty.", 0};
		}
		return ParallelForCancelable(
			Name,
			Num,
			[Function = std::move(Function)](uint64 Index, const FParallelForCancellationToken&) mutable {
				Function(Index);
			},
			Options
		);
	}

	auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		if (!Function)
		{
			return {ETaskState::Invalid, "ParallelFor function is empty.", 0};
		}
		if (Num == 0)
		{
			return {ETaskState::Succeeded, {}, 0};
		}
		if (Options.CancellationToken.IsCancellationRequested())
		{
			return {ETaskState::Canceled, "ParallelFor cancellation was requested before execution.", 0};
		}
		FTaskAttribution SelectedAttribution = Options.Attribution;
		if (Private::FTaskAttributionAccess::IsDefault(SelectedAttribution) && GCurrentTaskState && GCurrentTaskScheduler)
		{
			SelectedAttribution = GCurrentTaskState->GetAttribution();
		}
		uint32 SchedulerWorkerCount = 0;
		bool bSchedulerRunning = false;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running && GTaskScheduler)
			{
				bSchedulerRunning = true;
				SchedulerWorkerCount = GTaskScheduler->GetWorkerCount();
				GTaskScheduler->RecordParallelForOperation(SelectedAttribution);
			}
		}

		struct FParallelForDepthScope
		{
			FParallelForDepthScope() { ++GParallelForDepth; }
			~FParallelForDepthScope()
			{
				check(GParallelForDepth > 0);
				--GParallelForDepth;
			}
		};

		struct FSharedParallelForState
		{
			std::mutex Mutex;
			FTaskCancellationSource CancellationSource;
			std::shared_ptr<FCancelableParallelForFunction> Function;
			uint64 LowestFailedChunkStart = std::numeric_limits<uint64>::max();
			std::string FailureDiagnostic;
		};

		const bool bNested = GParallelForDepth > 0;
		FParallelForDepthScope CallerDepthScope;
		auto SharedState = std::make_shared<FSharedParallelForState>();
		SharedState->Function = std::make_shared<FCancelableParallelForFunction>(std::move(Function));
		const FTaskCancellationToken ExternalToken = Options.CancellationToken;

		auto RecordFailure = [SharedState](uint64 ChunkStart, uint64 ChunkEnd, std::string Diagnostic) {
			std::lock_guard Lock(SharedState->Mutex);
			if (ChunkStart < SharedState->LowestFailedChunkStart)
			{
				SharedState->LowestFailedChunkStart = ChunkStart;
				SharedState->FailureDiagnostic = "ParallelFor chunk [" + std::to_string(ChunkStart) + ", "
					+ std::to_string(ChunkEnd) + ") failed: " + std::move(Diagnostic);
			}
		};

		auto ExecuteChunk = [SharedState, ExternalToken, RecordFailure](uint64 ChunkStart, uint64 ChunkEnd, const FTaskCancellationToken& TaskToken) {
			FParallelForDepthScope ChunkDepthScope;
			const FParallelForCancellationToken CancellationToken(TaskToken, ExternalToken);
			for (uint64 Index = ChunkStart; Index < ChunkEnd; ++Index)
			{
				if (CancellationToken.IsCancellationRequested())
				{
					SharedState->CancellationSource.RequestCancellation();
					return;
				}
				try
				{
					(*SharedState->Function)(Index, CancellationToken);
				}
				catch (const std::exception& Exception)
				{
					RecordFailure(ChunkStart, ChunkEnd, Exception.what());
					SharedState->CancellationSource.RequestCancellation();
					throw;
				}
				catch (...)
				{
					RecordFailure(ChunkStart, ChunkEnd, "callable threw an unknown exception.");
					SharedState->CancellationSource.RequestCancellation();
					throw;
				}
			}
			if (CancellationToken.IsCancellationRequested())
			{
				SharedState->CancellationSource.RequestCancellation();
			}
		};

		const uint64 MinBatchSize = std::max<uint64>(1, Options.MinBatchSize);
		const uint64 BatchLimitedChunks = 1 + (Num - 1) / MinBatchSize;
		const uint64 WorkerLimitedChunks = bSchedulerRunning ? static_cast<uint64>(SchedulerWorkerCount) + 1 : 1;
		const uint32 ChunkCount = static_cast<uint32>(std::min<uint64>(Num, std::min(BatchLimitedChunks, WorkerLimitedChunks)));
		const uint32 EffectiveChunkCount = bNested ? 1 : std::max<uint32>(1, ChunkCount);

		auto GetChunkRange = [Num, EffectiveChunkCount](uint32 ChunkIndex) {
			const uint64 BaseChunkSize = Num / EffectiveChunkCount;
			const uint64 LargerChunkCount = Num % EffectiveChunkCount;
			const uint64 ChunkStart = static_cast<uint64>(ChunkIndex) * BaseChunkSize + std::min<uint64>(ChunkIndex, LargerChunkCount);
			const uint64 ChunkSize = BaseChunkSize + (ChunkIndex < LargerChunkCount ? 1 : 0);
			return std::pair<uint64, uint64>{ChunkStart, ChunkStart + ChunkSize};
		};

		std::vector<FTaskHandle> WorkerTasks;
		WorkerTasks.reserve(EffectiveChunkCount - 1);
		FTaskLaunchOptions LaunchOptions;
		LaunchOptions.CancellationToken = SharedState->CancellationSource.GetToken();
		LaunchOptions.Attribution = SelectedAttribution;
		LaunchOptions.Scope = Options.Scope;
		bool bLaunchFailed = false;
		for (uint32 ChunkIndex = 1; ChunkIndex < EffectiveChunkCount; ++ChunkIndex)
		{
			const auto [ChunkStart, ChunkEnd] = GetChunkRange(ChunkIndex);
			FTaskHandle Task = LaunchCancelableTask(
				Name ? Name : "ParallelFor",
				[ExecuteChunk, ChunkStart, ChunkEnd](const FTaskCancellationToken& TaskToken) {
					ExecuteChunk(ChunkStart, ChunkEnd, TaskToken);
				},
				LaunchOptions
			);
			if (!Task.IsValid())
			{
				bLaunchFailed = true;
				SharedState->CancellationSource.RequestCancellation();
				break;
			}
			WorkerTasks.emplace_back(std::move(Task));
		}

		const auto [CallerChunkStart, CallerChunkEnd] = GetChunkRange(0);
		try
		{
			ExecuteChunk(CallerChunkStart, CallerChunkEnd, SharedState->CancellationSource.GetToken());
		}
		catch (...)
		{
			// ExecuteChunk records the stable failure before preserving task-style exception precedence.
		}

		const std::vector<ETaskState> WorkerOutcomes = WaitAll(WorkerTasks);
		bool bAnyCanceled = bLaunchFailed || SharedState->CancellationSource.IsCancellationRequested() || Options.CancellationToken.IsCancellationRequested();
		for (ETaskState Outcome : WorkerOutcomes)
		{
			bAnyCanceled = bAnyCanceled || Outcome == ETaskState::Canceled || Outcome == ETaskState::Invalid;
		}

		{
			std::lock_guard Lock(SharedState->Mutex);
			if (!SharedState->FailureDiagnostic.empty())
			{
				return {ETaskState::Failed, SharedState->FailureDiagnostic, EffectiveChunkCount};
			}
		}
		if (bAnyCanceled)
		{
			return {
				ETaskState::Canceled,
				bLaunchFailed ? "ParallelFor could not launch every worker chunk." : "ParallelFor cancellation prevented full range coverage.",
				EffectiveChunkCount
			};
		}
		return {ETaskState::Succeeded, {}, EffectiveChunkCount};
	}
} // namespace Durin

#include "Threading/Task.h"
#include "Threading/Private/TaskSchedulerLifetime.h"

#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
#include "Threading/Private/QueuedWorkNode.h"
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


	}

	namespace
	{
		class FTaskWaitDeadline
		{
		public:
			explicit FTaskWaitDeadline(double Seconds) : TimeoutSeconds(Seconds >= 0 ? Seconds : 0) {}
			auto Remaining() const -> double
			{
				return std::max(0.0, TimeoutSeconds - std::chrono::duration<double>(
					std::chrono::steady_clock::now() - Start).count());
			}
			auto IsExpired() const -> bool { return Remaining() == 0; }
		private:
			std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
			double TimeoutSeconds;
		};

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
			PayloadBytes,
			Count,
		};

		enum class ETaskAggregateHistogram : uint8
		{
			QueueResidency,
			Execution,
			PayloadBytes,
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
				Out.CurrentPayloadBytes = Gauge(ETaskAggregateGauge::PayloadBytes);
				Out.PeakPayloadBytes = GaugePeak(ETaskAggregateGauge::PayloadBytes);
				for (size_t Bucket = 0; Bucket < 32; ++Bucket)
				{
					Out.QueueResidencyHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::QueueResidency)][Bucket].load(std::memory_order::acquire);
					Out.ExecutionHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::Execution)][Bucket].load(std::memory_order::acquire);
					Out.PayloadBytesHistogram[Bucket] = Histograms[AggregateIndex(ETaskAggregateHistogram::PayloadBytes)][Bucket].load(std::memory_order::acquire);
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
			uint64 PayloadBytes = 0;
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
		std::mutex GTaskSchedulerMutex;
		std::mutex GTaskDependencyMutex;
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
		thread_local bool GExecutingBlockingIOPool = false;
		thread_local uint32 GParallelForDepth = 0;
		thread_local bool GIsPumpingGameThreadDeferred = false;

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



	class FTaskCancellationState
	{
	public:
		auto IsCancellationRequested() const -> bool
		{
			return bCancellationRequested.load(std::memory_order_acquire);
		}

		auto RegisterTask(const std::shared_ptr<FTaskStateData>& Task) -> void;
		auto UnregisterTask(uint64 TaskId) -> void;
		auto RequestCancellation() -> void;

	private:
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::weak_ptr<FTaskStateData>> Tasks;
		std::atomic<bool> bCancellationRequested = false;
	};



	class FTaskGenerationState
	{
	public:
		std::atomic<uint64> Generation = 1;
	};

	// Owns one scheduler task node independently of public handles and pool work items.
	class FTaskStateData final : public Private::FQueuedWorkNode, public std::enable_shared_from_this<FTaskStateData>
	{
	public:
		FTaskStateData(
			const char* InDebugName,
			std::weak_ptr<FTaskScheduler> InScheduler,
			std::shared_ptr<FTaskSchedulerLifetimeAccounting> InLifetimeAccounting,
			std::unique_ptr<Private::FMoveOnlyTaskFunction> InFunction,
			Private::FTaskCompletionFunction&& InCompletionFunction,
			const FTaskCancellationToken& InCancellationToken,
			const std::vector<std::shared_ptr<FTaskStateData>>& InPrerequisites,
			uint64 InParentTaskId,
			ETaskDependencyKind InDependencyKind,
			bool bInAggregatePrerequisites,
			ETaskTarget InTarget,
			ETaskPriority InPriority,
			uint64 InEstimatedPayloadBytes,
			bool bInQueueOnSaturation,
			bool bInIndependentCPU,
			FTaskAttribution InAttribution,
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
			, RemainingPrerequisites(static_cast<uint32>(InPrerequisites.size()))
			, State(InPrerequisites.empty() ? ETaskState::Queued : ETaskState::Waiting)
			, DependencyKind(InDependencyKind)
			, bAggregatePrerequisites(bInAggregatePrerequisites)
			, Target(InTarget)
			, Priority(InPriority)
			, EstimatedPayloadBytes(InEstimatedPayloadBytes)
			, bQueueOnSaturation(bInQueueOnSaturation)
			, bIndependentCPU(bInIndependentCPU)
			, Attribution(InAttribution)
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
			ResultStorage.reset();
			ResultLifetimes.clear();
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
			}
		}

		auto RetainResult(std::shared_ptr<void> Result, bool bHasValue) -> void
		{
			std::lock_guard Lock(Mutex);
			require(!ResultStorage);
			ResultStorage = std::move(Result);
			bHasResultStorage = bHasValue;
			// Fast tasks can publish before the typed handle binds its result storage.
			if (bHasResultStorage && bTerminalPublicationFinished)
			{
				LifetimeAccounting->RetainedTerminalResultCount.fetch_add(1, std::memory_order::acq_rel);
				bTerminalResultLifetimeCharged = true;
			}
		}
		auto RetainLifetime(std::shared_ptr<void> Lifetime) -> void
		{
			std::lock_guard Lock(Mutex);
			ResultLifetimes.push_back(std::move(Lifetime));
		}

		auto InitializeOwnership(std::unique_ptr<Private::FMoveOnlyTaskFunction>& Function,
			Private::FTaskCompletionFunction& Completion) -> void
		{
			std::lock_guard Lock(Mutex);
			PendingFunction = std::move(Function);
			CompletionFunction = std::move(Completion);
		}
		auto ActivateAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAdmissionPending = false;
		}
		auto ReserveDependent() -> void
		{
			std::lock_guard Lock(Mutex);
			Dependents.reserve(Dependents.size() + ReservedDependents + 1);
			++ReservedDependents;
		}
		auto ReleaseDependentReservation() -> void
		{
			std::lock_guard Lock(Mutex);
			require(ReservedDependents > 0);
			--ReservedDependents;
		}

		auto ActivateDetachedCompletion() -> void
		{
			std::lock_guard Lock(Mutex);
			State = ETaskState::Running;
			bAdmissionPending = false;
			bExternalCompletion = true;
			bUnknownExecutionRequirement = true;
			bDetachedCompletion = true;
		}
		auto ConfigureExternal(bool bUnknown) -> void
		{
			std::lock_guard Lock(Mutex);
			bExternalCompletion = true;
			bUnknownExecutionRequirement = bUnknown;
		}
		auto IsRunningBody() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return State == ETaskState::Running && !bExternalCompletion && !DeferredExecution;
		}
		auto IsExternal() const -> bool { return bExternalCompletion; }
		auto IsDetachedCompletion() const -> bool { return bDetachedCompletion; }
		auto BindTerminal(std::shared_ptr<Private::FTaskTerminalHook> Hook) -> void
		{
			ETaskState Terminal;
			{
				std::lock_guard Lock(Mutex);
				if (!bTerminalHooksFinished)
				{
					Hook->Next = std::move(TerminalHooks);
					TerminalHooks = std::move(Hook);
					return;
				}
				Terminal = State;
			}
			Hook->Function(Terminal);
		}
		auto SetDynamicDependency(std::shared_ptr<FTaskStateData> Inner, bool bForwardCancellation) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (bDynamicDependencyBound) return false;
			DynamicPrerequisite = std::move(Inner);
			bForwardDynamicCancellation = bForwardCancellation;
			bDynamicDependencyBound = true;
			bUnknownExecutionRequirement = false;
			return true;
		}
		auto NotifyProgress() -> void;

		auto RegisterDependent(const std::shared_ptr<FTaskStateData>& Dependent) -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			require(ReservedDependents > 0);
			--ReservedDependents;
			CV.wait(Lock, [this]() { return !IsTerminalState(State) || bTerminalPublicationFinished; });
			if (!IsTerminalState(State))
			{
				Dependents.emplace_back(Dependent);
			}
			return State;
		}

		auto OnPrerequisiteTerminal(ETaskState PrerequisiteState, uint64 PrerequisiteTaskId) -> void;
		auto HasPendingFunction() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return PendingFunction && static_cast<bool>(*PendingFunction);
		}

		auto PrepareForQueue() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued || bAdmissionPending || DispatchTimeNanoseconds != 0)
			{
				return false;
			}
			DispatchTimeNanoseconds = MonotonicNanoseconds();
			return true;
		}

		auto Execute() -> void override;
		auto Discard() -> void override
		{ RequestCancellation("Task was discarded during scheduler shutdown.", ETaskTerminalReason::ShutdownCanceled); }
		auto GetName() const -> const char* override { return GetDebugName(); }

		auto TryMarkRunning(std::unique_ptr<Private::FMoveOnlyTaskFunction>* OutFunction = nullptr) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued)
			{
				return false;
			}
			// Execution and cancellation claim the callback under the same lock.
			if (OutFunction) *OutFunction = std::move(PendingFunction);
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
			return bCancellationRequested.load(std::memory_order_acquire);
		}

		auto DependsOn(const FTaskStateData* PotentialPrerequisite) const -> bool
		{
			std::vector<std::shared_ptr<FTaskStateData>> PendingPrerequisites;
			{
				std::lock_guard Lock(Mutex);
				PendingPrerequisites.reserve(Prerequisites.size() + 1);
				if (DynamicPrerequisite) PendingPrerequisites.emplace_back(DynamicPrerequisite);
				for (const auto& Nested : NestedPrerequisites)
					if (auto Pinned = Nested.lock()) PendingPrerequisites.emplace_back(std::move(Pinned));
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
				if (Prerequisite->DynamicPrerequisite) PendingPrerequisites.emplace_back(Prerequisite->DynamicPrerequisite);
				for (const auto& Nested : Prerequisite->NestedPrerequisites)
					if (auto Pinned = Nested.lock()) PendingPrerequisites.emplace_back(std::move(Pinned));
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

		// Inspect one node lock at a time. Terminal ancestors impose no executor
		// requirement; pinning each frontier keeps concurrent publication safe.
		auto IsIndependentCPU() const -> bool { return bIndependentCPU; }

		auto RequiresGameThread(bool bOnlyUnknown = false) const -> bool
		{
			// Most waits inspect a short chain. Pin and deduplicate that graph on
			// the stack; unusually wide/deep graphs use the unbounded traversal.
			std::array<std::shared_ptr<const FTaskStateData>, 16> Inline;
			Inline[0] = shared_from_this();
			size_t Count = 1;
			bool bOverflow = false;
			for (size_t Index = 0; Index < Count && !bOverflow; ++Index)
			{
				const auto& Current = Inline[Index];
				std::lock_guard Lock(Current->Mutex);
				if (IsTerminalState(Current->State) && Current->bTerminalPublicationFinished) continue;
				if ((!bOnlyUnknown && !Current->DeferredExecution && Current->Target == ETaskTarget::GameThreadDeferred) || Current->bUnknownExecutionRequirement) return true;
				auto Add = [&](std::shared_ptr<const FTaskStateData> Prerequisite) {
					if (!Prerequisite) return;
					for (size_t Seen = 0; Seen < Count; ++Seen)
						if (Inline[Seen] == Prerequisite) return;
					if (Count == Inline.size()) { bOverflow = true; return; }
					Inline[Count++] = std::move(Prerequisite);
				};
				Add(Current->DynamicPrerequisite);
				for (const auto& Nested : Current->NestedPrerequisites) Add(Nested.lock());
				for (const auto& Weak : Current->Prerequisites) Add(Weak.lock());
			}
			if (!bOverflow) return false;
			std::vector<std::shared_ptr<const FTaskStateData>> Pending{shared_from_this()};
			std::unordered_set<const FTaskStateData*> Visited;
			while (!Pending.empty())
			{
				auto Current = std::move(Pending.back());
				Pending.pop_back();
				if (!Visited.emplace(Current.get()).second) continue;
				std::lock_guard Lock(Current->Mutex);
				if (IsTerminalState(Current->State) && Current->bTerminalPublicationFinished) continue;
				if ((!bOnlyUnknown && !Current->DeferredExecution && Current->Target == ETaskTarget::GameThreadDeferred) || Current->bUnknownExecutionRequirement) return true;
				if (Current->DynamicPrerequisite) Pending.emplace_back(Current->DynamicPrerequisite);
				for (const auto& Nested : Current->NestedPrerequisites)
					if (auto Pinned = Nested.lock()) Pending.emplace_back(std::move(Pinned));
				for (const auto& WeakPrerequisite : Current->Prerequisites)
				{
					if (auto Prerequisite = WeakPrerequisite.lock())
						Pending.emplace_back(std::move(Prerequisite));
				}
			}
			return false;
		}

		// Phase counters retire retraction backlinks without duplicating persistent
		// graph metadata. Unfinished links remain available to concurrent waiters.
		auto AppendRetractionDependencies(std::vector<std::shared_ptr<FTaskStateData>>& Result) const -> void
		{
			std::lock_guard Lock(Mutex);
			if (IsTerminalState(State) && bTerminalPublicationFinished) return;
			if (DynamicPrerequisite) Result.push_back(DynamicPrerequisite);
			if (PendingNestedCount != 0)
				for (const auto& Weak : NestedPrerequisites)
					if (auto Pinned = Weak.lock()) Result.push_back(std::move(Pinned));
			if (RemainingPrerequisites != 0)
				for (const auto& Weak : Prerequisites)
					if (auto Pinned = Weak.lock()) Result.push_back(std::move(Pinned));
		}

		auto RequestCancellation(std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::CancellationRequested, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto OnNestedCompleted() -> void;
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
			auto Completed = [this] { return IsTerminalState(State) && bTerminalPublicationFinished; };
			if (std::isinf(TimeoutSeconds)) CV.wait(Lock, Completed);
			else CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), Completed);
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
				Snapshot.EstimatedStorageBytes = SchedulerStorageBytes;
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

		auto GetAccountingSnapshot() const -> FTaskAccountingSnapshot
		{
			std::lock_guard Lock(Mutex);
			return {
				.Attribution = Attribution,
				.State = State,
				.StateBeforeTerminal = StateBeforeTerminal,
				.TerminalReason = TerminalReason,
				.PayloadBytes = EstimatedPayloadBytes,
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
		auto GetEstimatedPayloadBytes() const -> uint64 { return EstimatedPayloadBytes; }
		auto QueuesOnSaturation() const -> bool { return bQueueOnSaturation; }
		auto SetSchedulerStorageBytes(uint64 Bytes) -> void { SchedulerStorageBytes = Bytes; }
		auto GetSchedulerStorageBytes() const -> uint64 { return SchedulerStorageBytes; }
		auto GetGenerationToken() const -> const FTaskGenerationToken& { return GenerationToken; }
		auto GetCoalescingKey() const -> const std::optional<FTaskCoalescingKey>& { return CoalescingKey; }

	private:
		auto PublishTerminal(ETaskState TerminalState, std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
		friend struct Private::FTaskRuntimeAccess;
		struct FDeferredExecution
		{
			ETaskState TerminalState;
			std::string Diagnostic;
			ETaskTerminalReason Reason;
			uint64 DirectBlockingTaskId;
		};
		std::optional<FDeferredExecution> DeferredExecution;
		auto PublishTerminalLocked(ETaskState TerminalState, std::string InDiagnostic, std::vector<std::shared_ptr<FTaskStateData>>& OutDependents, Private::FTaskCompletionFunction& OutCompletionFunction, std::unique_ptr<Private::FMoveOnlyTaskFunction>& OutPendingFunction, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
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
		Private::FTaskCompletionFunction CompletionFunction;
		std::shared_ptr<void> ResultStorage;
		std::vector<std::shared_ptr<void>> ResultLifetimes;
		bool bHasResultStorage = false;
		bool bAdmissionPending = true;
		size_t ReservedDependents = 0;
		bool bTerminalHooksFinished = false;
		bool bExternalCompletion = false;
		bool bDetachedCompletion = false;
		uint32 PendingNestedCount = 0;
		std::vector<std::weak_ptr<FTaskStateData>> NestedPrerequisites;
		std::vector<std::weak_ptr<FTaskStateData>> CompletionParents;
		bool bUnknownExecutionRequirement = false;
		std::shared_ptr<Private::FTaskTerminalHook> TerminalHooks;
		std::shared_ptr<FTaskStateData> DynamicPrerequisite;
		bool bForwardDynamicCancellation = true;
		bool bDynamicDependencyBound = false;
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
		bool bQueueOnSaturation = false;
		const bool bIndependentCPU;
		uint64 SchedulerStorageBytes = 0;
		FTaskAttribution Attribution;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		std::atomic<bool> bCancellationRequested = false;
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

	auto DispatchGameThreadDeferredTask(const std::shared_ptr<FTaskStateData>& State) -> bool;
	static std::atomic<int32> GTaskAdmissionAllocationFailure = 0;
	static auto CheckTaskAdmissionAllocation(int32 Checkpoint) -> void
	{
		int32 Expected = Checkpoint;
		if (GTaskAdmissionAllocationFailure.compare_exchange_strong(Expected, 0)) throw std::bad_alloc();
	}

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

			if (!Scheduler->BlockingIOPool.Create(Config.NumBlockingIOThreads, "EngineBlockingIO")) return nullptr;
			Scheduler->BlockingIOCapacity = Config.MaxBlockingIOTasks;
			Scheduler->WorkerCount = NumThreads;
			Scheduler->TaskReservationCapacity = Config.MaxNonterminalTasks;
			Scheduler->bAcceptingTasks = true;
			return Scheduler;
		}

		auto BeginSubmission() -> void
		{
			std::lock_guard Lock(Mutex);
			++PendingSubmissions;
		}
		auto EndSubmission() -> void
		{
			std::lock_guard Lock(Mutex);
			require(PendingSubmissions > 0);
			--PendingSubmissions;
			QuiescenceCV.notify_all();
		}
		auto WaitForSubmissions() -> void
		{
			std::unique_lock Lock(Mutex);
			QuiescenceCV.wait(Lock, [this] { return PendingSubmissions == 0; });
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
			Aggregate.Add(ETaskAggregateGauge::PayloadBytes, Task.PayloadBytes);
			Aggregate.Record(ETaskAggregateHistogram::PayloadBytes, Task.PayloadBytes);
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

		auto RecordParallelForOperation(FTaskAttribution Attribution) -> void
		{
			GetAggregate(Attribution).Increment(ETaskAggregateCounter::ParallelForOperation);
		}

		auto GetWorkerCount() const -> uint32 { return WorkerCount; }
		auto Submit(
			const char* Name,
			std::unique_ptr<Private::FMoveOnlyTaskFunction>& FunctionOwner,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			ETaskDependencyKind DependencyKind = ETaskDependencyKind::Success,
			bool bAggregatePrerequisites = false,
			ETaskTarget Target = ETaskTarget::AnyWorker,
			ETaskPriority Priority = ETaskPriority::Normal,
			uint64 EstimatedPayloadBytes = 0,
			uint64 AdditionalStorageBytes = 0,
			FTaskGenerationToken GenerationToken = {},
			std::optional<FTaskCoalescingKey> CoalescingKey = {},
			Tasks::FTaskAdmissionError* AdmissionError = nullptr) -> std::shared_ptr<FTaskStateData>
		{
			std::shared_ptr<FTaskStateData> State;
			size_t ReservedDependencyCount = 0;
			std::vector<std::shared_ptr<FTaskStateData>> PrerequisiteStates;
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingTasks)
				{
					RecordRejectedTask(Options.Attribution);
					if (AdmissionError) *AdmissionError = {Tasks::ETaskAdmissionErrorCode::LifetimeClosed, 0};
					return {};
				}

				if ((GCurrentTaskState && GCurrentTaskState->IsIndependentCPU())
					|| (Options.bIndependentCPU && (Target != ETaskTarget::AnyWorker
						|| Options.bExternalCompletion || !Options.Prerequisites.empty())))
				{
					RecordRejectedTask(Options.Attribution);
					if (AdmissionError) *AdmissionError = {Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
					return {};
				}

				for (const Tasks::FTask& Prerequisite : Options.Prerequisites)
				{
					if (!Prerequisite.State || Prerequisite.State->PinScheduler().get() != this)
					{
						RecordRejectedTask(Options.Attribution);
						if (AdmissionError) *AdmissionError = {Tasks::ETaskAdmissionErrorCode::InvalidPrerequisite, Prerequisite.GetTaskId()};
						return {};
					}
					PrerequisiteStates.emplace_back(Prerequisite.State);
				}
				if (!Options.bQueueOnSaturation && (CurrentTaskReservationCount.load(std::memory_order::acquire) >= TaskReservationCapacity
					|| (Target == ETaskTarget::BlockingIO && BlockingIOReservations >= BlockingIOCapacity)))
				{
					RecordCapacityRejectedTask(Options.Attribution);
					if (AdmissionError) *AdmissionError = {Tasks::ETaskAdmissionErrorCode::CapacityExhausted, 0};
					return {};
				}
				const uint64 CurrentReservations = CurrentTaskReservationCount.fetch_add(1, std::memory_order::acq_rel) + 1;

				uint64 PeakReservations = PeakTaskReservationCount.load(std::memory_order::acquire);
				while (PeakReservations < CurrentReservations
					&& !PeakTaskReservationCount.compare_exchange_weak(PeakReservations, CurrentReservations, std::memory_order::acq_rel)) {}

				try
				{
					State = std::make_shared<FTaskStateData>(
						Name,
						weak_from_this(),
						LifetimeAccounting,
						nullptr,
						Private::FTaskCompletionFunction{},
						Options.CancellationToken,
						PrerequisiteStates,
						GCurrentTaskState ? GCurrentTaskState->GetTaskId() : 0,
						DependencyKind,
						bAggregatePrerequisites,
						Target,
						Priority,
						EstimatedPayloadBytes,
						Options.bQueueOnSaturation,
						Options.bIndependentCPU,
						Options.Attribution,
						std::move(GenerationToken),
						std::move(CoalescingKey)
					);
					CheckTaskAdmissionAllocation(1);
					for (const auto& Prerequisite : PrerequisiteStates)
					{
						Prerequisite->ReserveDependent();
						++ReservedDependencyCount;
					}
					CheckTaskAdmissionAllocation(2);
					CheckTaskAdmissionAllocation(3);
					if (const auto& CancellationState = State->GetSharedCancellationState()) CancellationState->RegisterTask(State);
					CheckTaskAdmissionAllocation(4);
					ActiveTasks.emplace(State->GetTaskId(), State);
					CheckTaskAdmissionAllocation(5);
					State->SetSchedulerStorageBytes(sizeof(FTaskStateData) + sizeof(Private::FMoveOnlyTaskFunction)
						+ PrerequisiteStates.capacity() * sizeof(std::shared_ptr<FTaskStateData>) + AdditionalStorageBytes);
					State->InitializeOwnership(FunctionOwner, CompletionFunction);
					if (Options.bExternalCompletion) State->ConfigureExternal(Options.bUnknownExecutionRequirement);
					if (Target == ETaskTarget::BlockingIO) ++BlockingIOReservations;
				}
				catch (...)
				{
					const uint64 PreviousReservationCount = CurrentTaskReservationCount.fetch_sub(1, std::memory_order::acq_rel);
					require(PreviousReservationCount > 0);
					if (State)
					{
						ActiveTasks.erase(State->GetTaskId());
						if (const auto& CancellationState = State->GetSharedCancellationState()) CancellationState->UnregisterTask(State->GetTaskId());
					}
					for (size_t Index = 0; Index < ReservedDependencyCount; ++Index) PrerequisiteStates[Index]->ReleaseDependentReservation();
					throw;
				}
			}
			bool bReportOverload = false;
			{
				std::lock_guard Lock(Mutex);
				auto& Storage = ExecutorStorage[static_cast<size_t>(Target)];
				++Storage.CurrentNodes;
				++Storage.CurrentPendingNodes;
				Storage.CurrentPendingBytes += State->GetSchedulerStorageBytes();
				Storage.PeakPendingNodes = std::max(Storage.PeakPendingNodes, Storage.CurrentPendingNodes);
				Storage.PeakPendingBytes = std::max(Storage.PeakPendingBytes, Storage.CurrentPendingBytes);
				Storage.CurrentBytes += State->GetSchedulerStorageBytes();
				Storage.PeakNodes = std::max(Storage.PeakNodes, Storage.CurrentNodes);
				Storage.PeakBytes = std::max(Storage.PeakBytes, Storage.CurrentBytes);
				const uint64 Threshold = Target == ETaskTarget::BlockingIO ? BlockingIOCapacity : TaskReservationCapacity;
				if (Storage.CurrentNodes == Threshold + 1)
				{
					const uint64 Crossings = ++Storage.OverloadCrossings;
					bReportOverload = (Crossings & (Crossings - 1)) == 0;
				}
			}
			if (bReportOverload) DURIN_WARN("Task executor {} crossed its storage threshold; valid work remains queued.", static_cast<uint32>(Target));
			RecordAcceptedTask(State->GetAccountingSnapshot());
			Profiling::TaskEnqueued(
				State->GetTaskId(),
				Private::FTaskAttributionAccess::GetOwnerId(State->GetAttribution()),
				Private::FTaskAttributionAccess::GetCategoryId(State->GetAttribution()),
				static_cast<uint8>(State->GetTarget()));

			size_t RegisteredDependencies = 0;
			try
			{
				if (Options.bExternalCompletion)
				{
					const bool bStarted = State->TryMarkRunning();
					require(bStarted);
					OnTaskStarted(State->GetAccountingSnapshot());
				}
				for (const std::shared_ptr<FTaskStateData>& Prerequisite : PrerequisiteStates)
				{
					++RegisteredDependencies;
					const ETaskState PrerequisiteState = Prerequisite->RegisterDependent(State);
					if (IsTerminalState(PrerequisiteState))
					{
						State->OnPrerequisiteTerminal(PrerequisiteState, Prerequisite->GetTaskId());
					}
				}

				State->ActivateAdmission();
				CheckTaskAdmissionAllocation(6);
				if (State->IsCancellationRequested())
				{
					State->RequestCancellation("Task cancellation was requested during admission.");
				}
				else if (!Options.bExternalCompletion)
				{
					QueueTask(State);
				}
			}
			catch (const std::bad_alloc&)
			{
				requiref(!Options.bQueueOnSaturation, "Task registration allocation failed.");
				for (size_t Index = RegisteredDependencies; Index < PrerequisiteStates.size(); ++Index)
					PrerequisiteStates[Index]->ReleaseDependentReservation();
				State->ActivateAdmission();
				// Acceptance already transferred ownership; return a terminal handle, never a rejection.
				if (Options.bExternalCompletion) State->MarkFailed({});
				else State->RequestCancellation({}, ETaskTerminalReason::DispatchRejected);
			}

			return State;
		}

		auto QueueTask(const std::shared_ptr<FTaskStateData>& State) -> void
		{
			try
			{
				if (!State->PrepareForQueue())
				{
					return;
				}
				if (State->GetTarget() == ETaskTarget::GameThreadDeferred)
				{
					if (!DispatchGameThreadDeferredTask(State))
					{
						requiref(!State->QueuesOnSaturation() || IsTerminalState(State->GetState()), "Accepted task dispatch failed.");
					RecordRejectedTask(State->GetAttribution());
						State->RequestCancellation("GameThread deferred executor rejected task dispatch.", ETaskTerminalReason::DispatchRejected);
					}
					NotifyProgress();
					return;
				}

				auto& ExecutionPool = State->GetTarget() == ETaskTarget::BlockingIO ? BlockingIOPool : Pool;
				const bool bAccepted = ExecutionPool.EnqueueNode(State, static_cast<EQueuedWorkPriority>(State->GetPriority()));
				if (bAccepted) NotifyProgress();
				if (!bAccepted)
				{
					requiref(!State->QueuesOnSaturation() || IsTerminalState(State->GetState()), "Accepted task dispatch failed.");
					RecordRejectedTask(State->GetAttribution());
					State->RequestCancellation("Task could not be queued because scheduler shutdown had begun.", ETaskTerminalReason::DispatchRejected);
				}
			}
			catch (const std::bad_alloc&)
			{
				requiref(!State->QueuesOnSaturation(), "Task dispatch allocation failed.");
				State->RequestCancellation("Task dispatch allocation failed.", ETaskTerminalReason::DispatchRejected);
			}

		}

		auto ExecuteTask(
			const std::shared_ptr<FTaskStateData>& State,
			bool bWorkerExecution) -> void
		{
			std::unique_ptr<Private::FMoveOnlyTaskFunction> Function;
			if (!State->TryMarkRunning(&Function))
			{
				return;
			}
			{
				std::lock_guard Lock(Mutex);
				auto& Storage = ExecutorStorage[static_cast<size_t>(State->GetTarget())];
				--Storage.CurrentPendingNodes;
				Storage.CurrentPendingBytes -= State->GetSchedulerStorageBytes();
				++Storage.CurrentRunningBodies;
				Storage.PeakRunningBodies = std::max(Storage.PeakRunningBodies, Storage.CurrentRunningBodies);
			}
			OnTaskStarted(State->GetAccountingSnapshot());
			if (bWorkerExecution) OnWorkerStarted();
			DURIN_PROFILE_TASK_EXECUTION_ZONE(
				State->GetDebugName(),
				State->GetTaskId(),
				Private::FTaskAttributionAccess::GetOwnerId(State->GetAttribution()),
				Private::FTaskAttributionAccess::GetCategoryId(State->GetAttribution()),
				static_cast<uint8>(State->GetTarget()));

			FTaskStateData* PreviousTaskState = GCurrentTaskState;
			FTaskScheduler* PreviousTaskScheduler = GCurrentTaskScheduler;
			const bool PreviousBlockingIOPool = GExecutingBlockingIOPool;
			GExecutingBlockingIOPool = PreviousBlockingIOPool || State->GetTarget() == ETaskTarget::BlockingIO;
			GCurrentTaskState = State.get();
			GCurrentTaskScheduler = this;
			bool bBodyStorageReleased = false;
			auto ReleaseBodyStorage = [&] {
				if (bBodyStorageReleased) return;
				std::lock_guard Lock(Mutex);
				--ExecutorStorage[static_cast<size_t>(State->GetTarget())].CurrentRunningBodies;
				bBodyStorageReleased = true;
			};

			try
			{
				(*Function)(State->MakeCancellationToken());
				Function = {};
				ReleaseBodyStorage();
				NotifyProgress();
				State->MarkSucceeded();
			}
			catch (const std::exception& Exception)
			{
				Function = {};
				ReleaseBodyStorage();
				NotifyProgress();
				State->MarkFailed(Exception.what());
			}
			catch (...)
			{
				Function = {};
				ReleaseBodyStorage();
				NotifyProgress();
				State->MarkFailed("Task callable threw an unknown exception.");
			}

			GCurrentTaskState = PreviousTaskState;
			GCurrentTaskScheduler = PreviousTaskScheduler;
			GExecutingBlockingIOPool = PreviousBlockingIOPool;
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

				BlockingIOPool.StopAcceptingWork();
				Pool.Destroy(false);
				BlockingIOPool.Destroy(false);
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
				BlockingIOPool.Destroy(true);
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
			Aggregate.Subtract(ETaskAggregateGauge::PayloadBytes, Task.PayloadBytes);
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
				Private::FTaskAttributionAccess::GetOwnerId(Task.Attribution),
				Private::FTaskAttributionAccess::GetCategoryId(Task.Attribution),
				static_cast<uint8>(State->GetTarget()),
				static_cast<uint8>(Task.TerminalReason));

			std::lock_guard Lock(Mutex);
			if (State->GetTarget() == ETaskTarget::BlockingIO) { require(BlockingIOReservations > 0); --BlockingIOReservations; }
			auto& Storage = ExecutorStorage[static_cast<size_t>(State->GetTarget())];
			if (Task.StateBeforeTerminal != ETaskState::Running || State->IsExternal())
			{
				--Storage.CurrentPendingNodes;
				Storage.CurrentPendingBytes -= State->GetSchedulerStorageBytes();
			}
			--Storage.CurrentNodes;
			Storage.CurrentBytes -= State->GetSchedulerStorageBytes();
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
			NotifyProgress();
		}

		struct FRetractionWork
		{
			struct FFrame { std::shared_ptr<FTaskStateData> Task; bool bExecute = false; };
			std::vector<FFrame> Stack;
			std::vector<std::shared_ptr<FTaskStateData>> Dependencies;
			std::unordered_set<uint64> Visited;
		};

		auto RetractTask(const std::shared_ptr<FTaskStateData>& Target, const FTaskWaitDeadline& Deadline,
			FRetractionWork& Work) -> void
		{
			auto Execute = [&](const std::shared_ptr<FTaskStateData>& Task) {
				if (Task->GetTarget() == ETaskTarget::AnyWorker) Pool.TryExecuteNode(Task);
				else if (Task->GetTarget() == ETaskTarget::BlockingIO && GExecutingBlockingIOPool) BlockingIOPool.TryExecuteNode(Task);
			};
			Work.Stack.push_back({Target});
			while (!Work.Stack.empty() && !Deadline.IsExpired())
			{
				auto Frame = std::move(Work.Stack.back()); Work.Stack.pop_back();
				if (IsTerminalState(Frame.Task->GetState())) continue;
				if (!Frame.bExecute)
				{
					if (!Work.Visited.emplace(Frame.Task->GetTaskId()).second) continue;
					Work.Stack.push_back({Frame.Task, true});
				}
				else Execute(Frame.Task);
				// The second visit also sees dynamic/nested work introduced by the body.
				Frame.Task->AppendRetractionDependencies(Work.Dependencies);
				for (auto& Dependency : Work.Dependencies) Work.Stack.push_back({std::move(Dependency)});
				Work.Dependencies.clear();
			}
			// Release pins between passes; retain only allocation capacity for this Wait.
			Work.Stack.clear();
			Work.Visited.clear();
		}

		auto NotifyProgress() -> void
		{
			{ std::lock_guard Lock(ProgressMutex); ++ProgressSerial; }
			ProgressCV.notify_all();
		}
		auto PrepareProgressWait() -> uint64
		{ std::lock_guard Lock(ProgressMutex); return ProgressSerial; }
		auto WaitForProgress(uint64 Observed, double TimeoutSeconds) -> void
		{
			std::unique_lock Lock(ProgressMutex);
			auto Changed = [&] { return ProgressSerial != Observed; };
			if (std::isinf(TimeoutSeconds)) ProgressCV.wait(Lock, Changed);
			else ProgressCV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), Changed);
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

		auto RecordRejectedTask(FTaskAttribution Attribution = {}) -> void
		{
			RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Attribution);
			Aggregate.Increment(ETaskAggregateCounter::Rejected);
		}



		auto RecordCapacityRejectedTask(FTaskAttribution Attribution) -> void
		{
			RecordRejectedTask(Attribution);
			CapacityRejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			GetAggregate(Attribution).Increment(ETaskAggregateCounter::CapacityExhausted);
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
					Entry.CurrentPayloadBytes);
			}
		}

		auto GetDiagnostics(bool bInRunning) -> FTaskSchedulerDiagnostics
		{
			FTaskSchedulerDiagnostics Snapshot;
			Snapshot.WorkerCount = WorkerCount;
			{ std::lock_guard Lock(Mutex); Snapshot.ExecutorStorage = ExecutorStorage; }
			Snapshot.TaskReservationCapacity = TaskReservationCapacity;
			Snapshot.CurrentTaskReservationCount = CurrentTaskReservationCount.load(std::memory_order::acquire);
			Snapshot.PeakTaskReservationCount = PeakTaskReservationCount.load(std::memory_order::acquire);
			Snapshot.QueueDepth = Pool.GetNumQueuedTasks();
			Snapshot.BlockingIOWorkerCount = BlockingIOPool.GetNumThreads();
			Snapshot.BlockingIOQueueDepth = BlockingIOPool.GetNumQueuedTasks();
			Snapshot.BlockingIOCapacity = BlockingIOCapacity;
			{ std::lock_guard Lock(Mutex); Snapshot.BlockingIOReservations = BlockingIOReservations; }
			Snapshot.ActiveWorkerCount = ActiveWorkerCount.load(std::memory_order::acquire);
			Snapshot.CompletedTaskCount = CompletedTaskCount.load(std::memory_order::acquire);
			Snapshot.FailedTaskCount = FailedTaskCount.load(std::memory_order::acquire);
			Snapshot.CanceledTaskCount = CanceledTaskCount.load(std::memory_order::acquire);
			Snapshot.RejectedTaskCount = RejectedTaskCount.load(std::memory_order::acquire);
			Snapshot.CapacityRejectedTaskCount = CapacityRejectedTaskCount.load(std::memory_order::acquire);
			Snapshot.LongWaitCount = LongWaitCount.load(std::memory_order::acquire);
			Snapshot.AttributionRegistrationOverflowCount = GTaskAttributionRegistry.GetOverflowCount();
			Snapshot.OwnerCategoryDiagnostics = GTaskAttributionRegistry.Snapshot();
			for (FTaskOwnerCategoryDiagnostics& Entry : Snapshot.OwnerCategoryDiagnostics)
			{
				OwnerCategoryAggregates[Entry.CategoryId].Snapshot(Entry);
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
	private:
		FQueuedThreadPool Pool;
		FQueuedThreadPool BlockingIOPool;
		uint32 BlockingIOCapacity = 0;
		uint32 BlockingIOReservations = 0;
		mutable std::mutex Mutex;
		std::condition_variable QuiescenceCV;
		std::mutex ProgressMutex;
		std::condition_variable ProgressCV;
		uint64 ProgressSerial = 0;
		uint64 PendingSubmissions = 0;
		std::unordered_map<uint64, std::shared_ptr<FTaskStateData>> ActiveTasks;
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
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		uint32 WorkerCount = 0;
		std::array<FTaskExecutorStorageDiagnostics, 3> ExecutorStorage;
		uint64 TaskReservationCapacity = 0;
		bool bAcceptingTasks = false;
	};

	auto FTaskStateData::NotifyProgress() -> void
	{ if (auto Owner = PinScheduler()) Owner->NotifyProgress(); }

	auto FTaskStateData::Execute() -> void
	{
		if (auto Owner = PinScheduler()) Owner->ExecuteTask(shared_from_this(), GetTarget() == ETaskTarget::AnyWorker);
		else Discard();
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

		auto ValidatePayload(uint64 Bytes, bool bQueueOnSaturation) -> std::optional<Tasks::FTaskAdmissionError>
		{
			std::lock_guard Lock(Mutex);
			if (!bAccepting) return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::LifetimeClosed};
			if (!bQueueOnSaturation && (Bytes == 0 || Bytes > Config.MaxPayloadBytesPerEntry || Bytes > Config.MaxQueuedPayloadBytes))
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidPayloadDeclaration};
			return {};
		}

		auto Enqueue(const std::shared_ptr<FTaskStateData>& State) -> bool
		{
			auto Entry = std::make_shared<FEntry>();
			Entry->State = State;
			Entry->PayloadBytes = State->QueuesOnSaturation()
				? sizeof(FEntry) + State->GetSchedulerStorageBytes() : State->GetEstimatedPayloadBytes();
			Entry->Priority = State->GetPriority();
			Entry->GenerationToken = State->GetGenerationToken();
			Entry->CoalescingKey = State->GetCoalescingKey();
			Entry->EnqueueTimeNanoseconds = MonotonicNanoseconds();
			std::shared_ptr<FTaskStateData> SupersededState;
			std::shared_ptr<FEntry> SupersededEntry;
			std::vector<std::shared_ptr<FEntry>> DetachedEntries;
			bool bReportOverload = false;
			{
				std::lock_guard Lock(Mutex);
				PurgeTerminalEntriesLocked(DetachedEntries);
				const uint64 PayloadBytes = Entry->PayloadBytes;
				if (!bAccepting || (!State->QueuesOnSaturation() && (PayloadBytes == 0 || PayloadBytes > Config.MaxPayloadBytesPerEntry)))
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
				if (!State->QueuesOnSaturation() && (ActiveEntryCount - ReplacedEntries + 1 > Config.MaxQueuedEntries
					|| QueuedPayloadBytes - ReplacedBytes + PayloadBytes > Config.MaxQueuedPayloadBytes))
				{
					++Diagnostics.RejectedCount;
					return false;
				}

				const bool bWasOverloaded = ActiveEntryCount > Config.MaxQueuedEntries
					|| QueuedPayloadBytes > Config.MaxQueuedPayloadBytes;
				const bool bWillOverload = ActiveEntryCount - ReplacedEntries + 1 > Config.MaxQueuedEntries
					|| QueuedPayloadBytes - ReplacedBytes + PayloadBytes > Config.MaxQueuedPayloadBytes;
				if ((!bWasOverloaded && bWillOverload) || PayloadBytes > Config.MaxPayloadBytesPerEntry)
				{
					const uint64 Crossings = ++Diagnostics.OverloadCrossings;
					bReportOverload = (Crossings & (Crossings - 1)) == 0;
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

			if (bReportOverload) DURIN_WARN("Deferred task storage crossed a configured threshold; valid work remains queued.");
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
					Scheduler->ExecuteTask(Entry->State, false);
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

	auto Private::ValidateTaskExecution(ETaskTarget Target, ETaskPriority Priority, uint64 PayloadBytes, bool bQueueOnSaturation)
		-> std::optional<Tasks::FTaskAdmissionError>
	{
		using ECode = Tasks::ETaskAdmissionErrorCode;
		if ((Target != ETaskTarget::AnyWorker && Target != ETaskTarget::GameThreadDeferred && Target != ETaskTarget::BlockingIO)
			|| (Priority != ETaskPriority::High && Priority != ETaskPriority::Normal && Priority != ETaskPriority::Low))
			return Tasks::FTaskAdmissionError{ECode::UnsupportedExecutor};
		if (Target == ETaskTarget::AnyWorker || Target == ETaskTarget::BlockingIO) return {};
		if (!bQueueOnSaturation && PayloadBytes == 0) return Tasks::FTaskAdmissionError{ECode::InvalidPayloadDeclaration};
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		if (!Queue) return Tasks::FTaskAdmissionError{ECode::UnsupportedExecutor};
		return Queue->ValidatePayload(PayloadBytes, bQueueOnSaturation);
	}

	auto DispatchGameThreadDeferredTask(
		const std::shared_ptr<FTaskStateData>& State) -> bool
	{
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		return Queue && Queue->Enqueue(State);
	}

	auto FTaskStateData::PublishTerminalLocked(
		ETaskState TerminalState,
		std::string InDiagnostic,
		std::vector<std::shared_ptr<FTaskStateData>>& OutDependents,
		Private::FTaskCompletionFunction& OutCompletionFunction,
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
		Private::FTaskCompletionFunction Function;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		{
			std::lock_guard Lock(Mutex);
			if (IsTerminalState(State) || DeferredExecution) return false;
			if (PendingNestedCount != 0)
			{
				DeferredExecution.emplace(FDeferredExecution{TerminalState, std::move(InDiagnostic), InReason, InDirectBlockingTaskId});
				return true;
			}
			if (TerminalState == ETaskState::Succeeded && (IsCancellationRequested() || (SharedCancellationState && SharedCancellationState->IsCancellationRequested())))
			{
				TerminalState = ETaskState::Canceled;
				InDiagnostic = CancellationDiagnostic.empty() ? "Task returned after cancellation was requested." : CancellationDiagnostic;
				InReason = CancellationReason;
				InDirectBlockingTaskId = CancellationDirectBlockingTaskId;
			}
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
		for (;;)
		{
			std::shared_ptr<Private::FTaskTerminalHook> Hooks;
			{
				std::lock_guard Lock(Mutex);
				Hooks = std::move(TerminalHooks);
				if (!Hooks) bTerminalHooksFinished = true;
			}
			if (!Hooks) break;
			while (Hooks)
			{
				auto Hook = std::move(Hooks);
				Hooks = std::move(Hook->Next);
				Hook->Function(TerminalState);
			}
		}
		std::vector<std::weak_ptr<FTaskStateData>> Parents;
		{
			std::lock_guard Lock(Mutex);
			require(!bTerminalLifetimeCharged);
			LifetimeAccounting->RetainedTerminalTaskCount.fetch_add(1, std::memory_order::acq_rel);
			bTerminalLifetimeCharged = true;
			if (bHasResultStorage)
			{
				LifetimeAccounting->RetainedTerminalResultCount.fetch_add(1, std::memory_order::acq_rel);
				bTerminalResultLifetimeCharged = true;
			}
			bTerminalPublicationFinished = true;
			Parents = std::move(CompletionParents);
			NestedPrerequisites.clear();
		}
		CV.notify_all();
		NotifyProgress();
		for (const std::shared_ptr<FTaskStateData>& Dependent : Dependents)
		{
			Dependent->OnPrerequisiteTerminal(TerminalState, TaskId);
		}
		for (const auto& Parent : Parents)
			if (auto Pinned = Parent.lock()) Pinned->OnNestedCompleted();

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
		Private::FTaskCompletionFunction Function;
		std::unique_ptr<Private::FMoveOnlyTaskFunction> PendingFunction;
		bool bPublishedTerminal = false;
		std::shared_ptr<FTaskStateData> InnerToCancel;
		{
			std::lock_guard Lock(Mutex);
			if (IsTerminalState(State))
			{
				return false;
			}

			bCancellationRequested = true;
			if (bForwardDynamicCancellation) InnerToCancel = DynamicPrerequisite;
			if (CancellationDiagnostic.empty())
			{
				CancellationDiagnostic = std::move(InDiagnostic);
				CancellationReason = InReason;
				CancellationDirectBlockingTaskId = InDirectBlockingTaskId;
			}
			if (!bAdmissionPending && (State == ETaskState::Waiting || State == ETaskState::Queued))
			{
				bPublishedTerminal = PublishTerminalLocked(ETaskState::Canceled, CancellationDiagnostic, DependentsToNotify, Function, PendingFunction, CancellationReason, CancellationDirectBlockingTaskId);
			}
		}

		if (InnerToCancel) InnerToCancel->RequestCancellation("Outer task cancellation requested.");
		if (bPublishedTerminal)
		{
			InvokeTaskTerminalPublicationTestHook(TaskId);
			if (Function) Function(ETaskState::Canceled);
			Function = {};
			PendingFunction.reset();
			FinishTerminalPublication(ETaskState::Canceled, std::move(DependentsToNotify));
		}
		return true;
	}

	auto FTaskStateData::OnNestedCompleted() -> void
	{
		std::optional<FDeferredExecution> Execution;
		{
			std::lock_guard Lock(Mutex);
			require(PendingNestedCount != 0);
			if (--PendingNestedCount != 0) return;
			Execution = std::move(DeferredExecution);
			DeferredExecution.reset();
		}
		if (Execution) PublishTerminal(Execution->TerminalState, std::move(Execution->Diagnostic), Execution->Reason, Execution->DirectBlockingTaskId);
	}

	auto FTaskStateData::MarkSucceeded() -> void
	{
		PublishTerminal(ETaskState::Succeeded, {});
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

	auto Private::MakeVoidTaskResultState() -> std::shared_ptr<TTaskResultState<std::monostate>>
	{ return std::make_shared<TTaskResultState<std::monostate>>(); }

	Tasks::Private::FTaskHandle::FTaskHandle() = default;

	Tasks::Private::FTaskHandle::FTaskHandle(std::shared_ptr<FTaskStateData> InState)
		: State(std::move(InState))
	{
	}

	auto Tasks::Private::FTaskHandle::IsValid() const -> bool
	{
		return State != nullptr;
	}

	auto Tasks::Private::FTaskHandle::IsCompleted() const -> bool
	{
		return !State || IsTerminalState(State->GetState());
	}

	auto Tasks::Private::FTaskHandle::GetState() const -> ETaskState
	{
		return State ? State->GetState() : ETaskState::Invalid;
	}

	auto Tasks::Private::FTaskHandle::GetDebugName() const -> const char*
	{
		return State ? State->GetDebugName() : "";
	}

	auto Tasks::Private::FTaskHandle::GetTaskId() const -> uint64
	{
		return State ? State->GetTaskId() : 0;
	}

	auto Tasks::Private::FTaskHandle::GetDiagnostic() const -> std::string
	{
		return State ? State->GetDiagnostic() : std::string{};
	}

	auto Tasks::Private::FTaskHandle::GetDiagnostics() const -> FTaskDiagnostics
	{
		return State ? State->GetDiagnostics() : FTaskDiagnostics{};
	}

	auto InitializeTaskScheduler(uint32 InNumThreads) -> bool
	{
		return InitializeTaskScheduler(FTaskSchedulerConfig{.NumWorkerThreads = InNumThreads});
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
		if (Config.MaxNonterminalTasks == 0 || Config.MaxNonterminalTasks > std::numeric_limits<uint32>::max()
			|| Config.NumBlockingIOThreads == 0 || Config.MaxBlockingIOTasks == 0)
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
		SchedulerToDestroy->WaitForSubmissions();

		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}

		if (Mode == ETaskShutdownMode::Cancel)
		{
			// Accepted dependencies can become ready while cancellation releases
			// captures or waits for external producers. Retain internal dispatch
			// until the scheduler has terminalized the entire accepted graph.
			SchedulerToDestroy->Shutdown(false);
			if (Queue) Queue->CancelAll();
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
		SchedulerToDestroy->WaitForSubmissions();

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

	auto Private::GetTaskSchedulerLifetime() -> std::weak_ptr<const void>
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		return GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running
			? std::weak_ptr<const void>(GTaskScheduler) : std::weak_ptr<const void>{};
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


	namespace Private
	{
		auto SetTaskAdmissionAllocationFailureForTests(int32 Checkpoint) -> void
		{
			GTaskAdmissionAllocationFailure.store(Checkpoint);
		}

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

		// Covers preparation, publication, and rejected callable destruction during shutdown.
		struct FSubmissionGuard
		{
			std::shared_ptr<FTaskScheduler> Scheduler;
			~FSubmissionGuard() { if (Scheduler) Scheduler->EndSubmission(); }
		};

		auto TryLaunchCancelableTaskWithCompletion(
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			uint64 AdditionalStorageBytes) -> Tasks::TTaskAdmission<Tasks::FTask>
		try
		{
			using FAdmission = Tasks::TTaskAdmission<Tasks::FTask>;
			{
				if (auto Error = ValidateTaskExecution(Options.Target, Options.Priority, Options.EstimatedPayloadBytes, Options.bQueueOnSaturation))
					return FAdmission::Failure(*Error);
			}
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
					GTaskScheduler->RecordRejectedTask(ResolvedOptions.Attribution);
				}
				DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
				return FAdmission::Failure({Tasks::ETaskAdmissionErrorCode::InvalidCallable});
			}
			FSubmissionGuard Submission;
			auto FunctionOwner = std::make_unique<FMoveOnlyTaskFunction>(std::move(Function));

			{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask(ResolvedOptions.Attribution);
				}
				DURIN_WARN("Task launch failed because the task scheduler is not running. (task: {})", Name ? Name : "");
				return FAdmission::Failure({Tasks::ETaskAdmissionErrorCode::LifetimeClosed});
			}

			Submission.Scheduler = GTaskScheduler;
			Submission.Scheduler->BeginSubmission();
			}
			Tasks::FTaskAdmissionError AdmissionError{Tasks::ETaskAdmissionErrorCode::LifetimeClosed};
			std::shared_ptr<FTaskStateData> State = Submission.Scheduler->Submit(
				Name,
				FunctionOwner,
				std::move(CompletionFunction),
				ResolvedOptions,
				ETaskDependencyKind::Success,
				false,
				Options.Target,
				Options.Priority,
				Options.EstimatedPayloadBytes,
				AdditionalStorageBytes, Options.GenerationToken, Options.CoalescingKey, &AdmissionError
			);
			if (!State)
			{
				DURIN_WARN("Task launch failed because its prerequisites were invalid or scheduler admission was closed. (task: {})", Name ? Name : "");
				return FAdmission::Failure(AdmissionError);
			}
			return FAdmission::Success(Tasks::FTask(std::move(State)));
		}
		catch (const std::bad_alloc&)
		{
			return Tasks::TTaskAdmission<Tasks::FTask>::Failure({Tasks::ETaskAdmissionErrorCode::CapacityExhausted});
		}


		auto TryLaunchContinuationTask(
			const Tasks::FTask& Predecessor,
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskContinuationOptions& Options,
			ETaskDependencyKind DependencyKind,
			uint64 AdditionalStorageBytes) -> Tasks::TTaskAdmission<Tasks::FTask>
		try
		{
			using FAdmission = Tasks::TTaskAdmission<Tasks::FTask>;
			FTaskAttribution ResolvedAttribution = FTaskAttributionAccess::IsDefault(Options.Attribution) && Predecessor.State
				? Predecessor.State->GetAttribution()
				: Options.Attribution;
			{
				if (auto Error = ValidateTaskExecution(Options.Target, Options.Priority, Options.EstimatedPayloadBytes, Options.bQueueOnSaturation))
				{
					std::lock_guard Lock(GTaskSchedulerMutex);
					if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(ResolvedAttribution);
					return FAdmission::Failure(*Error);
				}
			}
			if (!Function)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(ResolvedAttribution);
				return FAdmission::Failure({Tasks::ETaskAdmissionErrorCode::InvalidCallable});
			}
			FSubmissionGuard Submission;
			auto FunctionOwner = std::make_unique<FMoveOnlyTaskFunction>(std::move(Function));
			std::vector<Tasks::FTask> Prerequisites;
			Prerequisites.reserve(Options.Prerequisites.size() + 1);
			auto AppendUnique = [&Prerequisites](const Tasks::FTask& Candidate) {
				if (std::ranges::none_of(Prerequisites, [&Candidate](const Tasks::FTask& Existing) {
					return Existing.GetTaskId() == Candidate.GetTaskId();
				}))
				{
					Prerequisites.emplace_back(Candidate);
				}
			};
			AppendUnique(Predecessor);
			for (const Tasks::FTask& Prerequisite : Options.Prerequisites) AppendUnique(Prerequisite);

			FTaskLaunchOptions LaunchOptions;
			LaunchOptions.Prerequisites = Prerequisites;
			LaunchOptions.bQueueOnSaturation = Options.bQueueOnSaturation;
			LaunchOptions.CancellationToken = Options.CancellationToken;
			LaunchOptions.Attribution = ResolvedAttribution;

			{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask(ResolvedAttribution);
				return FAdmission::Failure({Tasks::ETaskAdmissionErrorCode::LifetimeClosed});
			}
			Submission.Scheduler = GTaskScheduler;
			Submission.Scheduler->BeginSubmission();
			}
			Tasks::FTaskAdmissionError AdmissionError{Tasks::ETaskAdmissionErrorCode::LifetimeClosed};
			std::shared_ptr<FTaskStateData> State = Submission.Scheduler->Submit(
				Name,
				FunctionOwner,
				std::move(CompletionFunction),
				LaunchOptions,
				DependencyKind,
				true,
				Options.Target,
				Options.Priority,
				Options.EstimatedPayloadBytes,
				AdditionalStorageBytes,
				Options.GenerationToken,
				Options.CoalescingKey,
				&AdmissionError
			);
			return State ? FAdmission::Success(Tasks::FTask(std::move(State))) : FAdmission::Failure(AdmissionError);
		}
		catch (const std::bad_alloc&)
		{
			return Tasks::TTaskAdmission<Tasks::FTask>::Failure({Tasks::ETaskAdmissionErrorCode::CapacityExhausted});
		}


		auto FTaskRuntimeAccess::RetainResult(const Tasks::FTask& Task, std::shared_ptr<void> Result, bool bHasValue) -> void
		{ require(Task.State && Result); Task.State->RetainResult(std::move(Result), bHasValue); }
		auto FTaskRuntimeAccess::PinLifetime(const Tasks::FTask& Task) -> std::shared_ptr<void>
		{ require(Task.State); return Task.State; }
		auto FTaskRuntimeAccess::RetainLifetime(const Tasks::FTask& Task, std::shared_ptr<void> Lifetime) -> void
		{ require(Task.State); Task.State->RetainLifetime(std::move(Lifetime)); }

		auto FTaskRuntimeAccess::GetAttribution(const Tasks::FTask& Task) -> FTaskAttribution { return Task.State ? Task.State->GetAttribution() : FTaskAttribution{}; }

		auto FTaskRuntimeAccess::IsExecutingIndependentCPU() -> bool { return GCurrentTaskState && GCurrentTaskState->IsIndependentCPU(); }
		auto FTaskRuntimeAccess::GetCurrentTaskId() -> uint64 { return GCurrentTaskState && GCurrentTaskState->IsRunningBody() ? GCurrentTaskState->GetTaskId() : 0; }



		auto FTaskRuntimeAccess::CreateDetachedCompletion(FTaskCompletionFunction&& Completion) -> Tasks::FTask
		{
			auto State = std::make_shared<FTaskStateData>("DetachedCompletion", std::weak_ptr<FTaskScheduler>{},
				std::make_shared<FTaskSchedulerLifetimeAccounting>(), nullptr, std::move(Completion),
				FTaskCancellationToken{}, std::vector<std::shared_ptr<FTaskStateData>>{}, 0,
				ETaskDependencyKind::Completion, false, ETaskTarget::AnyWorker, ETaskPriority::Normal,
				0, false, false, FTaskAttribution{}, FTaskGenerationToken{}, std::nullopt);
			State->ActivateDetachedCompletion();
			return Tasks::FTask(std::move(State));
		}

		auto FTaskRuntimeAccess::BindTerminal(const Tasks::FTask& Task, std::shared_ptr<FTaskTerminalHook> Hook) -> void
		{
			require(Task.State && Hook && Hook->Function);
			Task.State->BindTerminal(std::move(Hook));
		}
		auto FTaskRuntimeAccess::CompleteExternal(const Tasks::FTask& Task, ETaskState Terminal) -> void
		{
			require(Task.State && Task.State->IsExternal());
			if (Terminal == ETaskState::Failed) Task.State->MarkFailed("External task source failed or was abandoned.");
			else
			{
				if (Terminal == ETaskState::Canceled) Task.State->RequestCancellation("External task cancellation acknowledged.");
				Task.State->MarkSucceeded();
			}
		}

		auto FTaskRuntimeAccess::BindDynamicDependency(const Tasks::FTask& Task, const Tasks::FTask& Inner, bool bForwardCancellation)
			-> std::optional<Tasks::FTaskAdmissionError>
		{
			// Serializes dynamic edge insertion and cycle checks across concurrent sources.
			{
				std::lock_guard Lock(GTaskDependencyMutex);
				if (!Task.State || !Inner.State || Task.State->PinScheduler() != Inner.State->PinScheduler())
					return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidPrerequisite, Inner.GetTaskId()};
				if (Task.State->IsIndependentCPU())
					return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
				if (Task.State == Inner.State || Inner.State->DependsOn(Task.State.get()))
					return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::DependencyCycle, Inner.GetTaskId()};
				if (!Task.State->SetDynamicDependency(Inner.State, bForwardCancellation))
					return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
			}
			// Cancellation can invoke terminal hooks, so it must happen outside dependency synchronization.
			Task.State->NotifyProgress();
			if (bForwardCancellation && Task.State->IsCancellationRequested())
				Inner.State->RequestCancellation("Outer task cancellation preceded inner binding.");
			return {};
		}

		auto FTaskRuntimeAccess::AddCompletionDependency(const Tasks::FTask& Parent, const Tasks::FTask& Child)
			-> std::optional<Tasks::FTaskAdmissionError>
		{
			std::lock_guard GraphLock(GTaskDependencyMutex);
			if (!Parent.State || !Child.State || Parent.State->PinScheduler() != Child.State->PinScheduler())
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidPrerequisite, Child.GetTaskId()};
			if (Parent.State->IsIndependentCPU())
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
			// A completed child imposes no dependency, including an already-canceled back-edge.
			if (IsTerminalState(Child.State->GetState()))
			{
				std::lock_guard Lock(Parent.State->Mutex);
				if (Parent.State->State != ETaskState::Running || Parent.State->DeferredExecution)
					return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
				return {};
			}
			if (Parent.State == Child.State || Child.State->DependsOn(Parent.State.get()))
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::DependencyCycle, Child.GetTaskId()};
			std::scoped_lock Lock(Parent.State->Mutex, Child.State->Mutex);
			if (Parent.State->State != ETaskState::Running || Parent.State->DeferredExecution)
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
			if (IsTerminalState(Child.State->State) && Child.State->bTerminalPublicationFinished) return {};
			// Reserve both sides before publishing the edge so allocation failure cannot leave a half-edge.
			Parent.State->NestedPrerequisites.reserve(Parent.State->NestedPrerequisites.size() + 1);
			Child.State->CompletionParents.reserve(Child.State->CompletionParents.size() + 1);
			Parent.State->NestedPrerequisites.emplace_back(Child.State);
			Child.State->CompletionParents.emplace_back(Parent.State);
			++Parent.State->PendingNestedCount;
			Parent.State->NotifyProgress();
			return {};
		}

		auto FTaskRuntimeAccess::AddNested(const Tasks::FTask& Child) -> std::optional<Tasks::FTaskAdmissionError>
		{
			if (!GCurrentTaskState || !GCurrentTaskState->IsRunningBody())
				return Tasks::FTaskAdmissionError{Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract};
			return AddCompletionDependency(Tasks::FTask(GCurrentTaskState->shared_from_this()), Child);
		}

		auto FTaskRuntimeAccess::IsCancellationRequested(const Tasks::FTask& Task) -> bool
		{
			return Task.State && Task.State->MakeCancellationToken().IsCancellationRequested();
		}

		auto FTaskRuntimeAccess::CancelCurrent() -> void
		{
			require(GCurrentTaskState);
			GCurrentTaskState->RequestCancellation("Task propagated prerequisite cancellation.");
		}


	} // namespace Private

	auto CancelTask(const Tasks::FTask& Task) -> bool
	{
		return Task.State && !Task.State->IsDetachedCompletion() && Task.State->RequestCancellation("Task cancellation was requested through its handle.");
	}

	auto WaitTask(const Tasks::FTask& Task) -> FTaskWaitResult
	{ return WaitTask(Task, std::numeric_limits<double>::infinity()); }

	namespace
	{
		auto GetTaskWaitRestriction(const std::shared_ptr<FTaskStateData>& TaskState) -> std::optional<ETaskWaitStatus>
		{
			if (GCurrentTaskState == TaskState.get())
			{
				DURIN_WARN("Task wait rejected because a task cannot wait for itself. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::SelfWait;
			}

			if (GCurrentTaskState && TaskState->DependsOn(GCurrentTaskState))
			{
				DURIN_WARN("Task wait rejected because the target depends on the current task. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::DependencyCycle;
			}

			if (GCurrentTaskState && GCurrentTaskState->IsIndependentCPU())
				return ETaskWaitStatus::UnsupportedThread;
			if (IsInRenderingThread() && !TaskState->IsIndependentCPU())
			{
				DURIN_WARN("Task wait rejected on the rendering thread. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::UnsupportedThread;
			}
			if (GCurrentTaskState && TaskState->RequiresGameThread(true))
				return ETaskWaitStatus::UnsupportedThread;
			if (GIsGameThreadIdInitialized && IsInGameThread() && TaskState->RequiresGameThread())
			{
				DURIN_WARN("Task wait rejected because GameThread cannot block on deferred GameThread work. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::UnsupportedThread;
			}

			return std::nullopt;
		}

		auto WaitTaskState(const std::shared_ptr<FTaskStateData>& TaskState, const FTaskWaitDeadline& Deadline) -> FTaskWaitResult
		{
			if (!TaskState)
			{
				return {ETaskWaitStatus::Completed, ETaskState::Invalid};
			}

			ETaskState State = TaskState->GetState();
			if (IsTerminalState(State))
			{
				return {ETaskWaitStatus::Completed, State};
			}
			// Polling never executes user code or requires authority to block.
			if (Deadline.IsExpired()) return {ETaskWaitStatus::TimedOut, State};

			if (auto Restriction = GetTaskWaitRestriction(TaskState)) return {*Restriction, State};

			std::shared_ptr<FTaskScheduler> Scheduler = TaskState->PinScheduler();
			const auto WaitStartTime = std::chrono::steady_clock::now();
			bool bReportedLongWait = false;
			auto ReportLongWait = [&] {
				if (bReportedLongWait || std::chrono::duration<double>(std::chrono::steady_clock::now() - WaitStartTime).count() < LongWaitThresholdSeconds)
				{
					return;
				}
				bReportedLongWait = true;
				if (Scheduler)
				{
					const char* WaiterName = GCurrentTaskState
						? GCurrentTaskState->GetDebugName()
						: (GetCurrentThread() ? GetCurrentThread()->GetThreadName() : "ExternalThread");
					const FTaskDiagnostics TargetDiagnostics = TaskState->GetDiagnostics();
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
			const bool bCanHelp = GCurrentTaskState && Scheduler && GCurrentTaskScheduler == Scheduler.get();
			FTaskScheduler::FRetractionWork Retraction;
			while (!IsTerminalState(State))
			{
				// Prepare before inspecting/retracting so a concurrent enqueue, binding
				// or publication between inspection and sleep cannot be lost.
				const uint64 Observed = Scheduler ? Scheduler->PrepareProgressWait() : 0;
				if (bCanHelp && !Deadline.IsExpired())
					Scheduler->RetractTask(TaskState, Deadline, Retraction);
				State = TaskState->GetState();
				ReportLongWait();
				if (IsTerminalState(State)) break;
				if (Deadline.IsExpired()) return {ETaskWaitStatus::TimedOut, State};
				const double UntilDiagnostic = bReportedLongWait ? std::numeric_limits<double>::infinity()
					: std::max(0.0, LongWaitThresholdSeconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - WaitStartTime).count());
				const double Timeout = std::min(Deadline.Remaining(), UntilDiagnostic);
				if (Scheduler) Scheduler->WaitForProgress(Observed, Timeout);
				else State = TaskState->WaitFor(Timeout);
			}
			return {ETaskWaitStatus::Completed, State};
		}
	}

	auto Tasks::Private::FTaskHandle::TryRetractAndExecute() const -> bool
	{
		if (!State || IsTerminalState(State->GetState())) return true;
		if (GetTaskWaitRestriction(State)) return false;
		if (auto Scheduler = State->PinScheduler())
		{
			FTaskScheduler::FRetractionWork Work;
			Scheduler->RetractTask(State, FTaskWaitDeadline(std::numeric_limits<double>::infinity()), Work);
		}
		return IsCompleted();
	}

	auto WaitTask(const Tasks::FTask& Task, double TimeoutSeconds) -> FTaskWaitResult
	{ return WaitTaskState(Task.State, FTaskWaitDeadline(TimeoutSeconds)); }

	auto WaitAll(std::span<const Tasks::FTask> Tasks) -> std::vector<FTaskWaitResult>
	{ return WaitAll(Tasks, std::numeric_limits<double>::infinity()); }

	auto WaitAll(std::span<const Tasks::FTask> Tasks, double TimeoutSeconds) -> std::vector<FTaskWaitResult>
	{
		const FTaskWaitDeadline Deadline(TimeoutSeconds);
		std::vector<FTaskWaitResult> Outcomes;
		Outcomes.reserve(Tasks.size());
		for (const Tasks::FTask& Task : Tasks)
			Outcomes.push_back(WaitTaskState(Task.State, Deadline));
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

		std::vector<Tasks::FTask> WorkerTasks;
		WorkerTasks.reserve(EffectiveChunkCount - 1);
		FTaskLaunchOptions LaunchOptions;
		LaunchOptions.CancellationToken = SharedState->CancellationSource.GetToken();
		LaunchOptions.Attribution = SelectedAttribution;
		bool bLaunchFailed = false;
		for (uint32 ChunkIndex = 1; ChunkIndex < EffectiveChunkCount; ++ChunkIndex)
		{
			const auto [ChunkStart, ChunkEnd] = GetChunkRange(ChunkIndex);
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion(
				Name ? Name : "ParallelFor",
				[ExecuteChunk, ChunkStart, ChunkEnd](const FTaskCancellationToken& TaskToken) {
					ExecuteChunk(ChunkStart, ChunkEnd, TaskToken);
				}, {},
				LaunchOptions
			);
			if (!Admission.HasValue())
			{
				bLaunchFailed = true;
				SharedState->CancellationSource.RequestCancellation();
				break;
			}
			WorkerTasks.emplace_back(std::move(Admission).TakeValue());
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

		const std::vector<FTaskWaitResult> WorkerOutcomes = WaitAll(WorkerTasks);
		bool bAnyCanceled = bLaunchFailed || SharedState->CancellationSource.IsCancellationRequested() || Options.CancellationToken.IsCancellationRequested();
		for (const FTaskWaitResult& Outcome : WorkerOutcomes)
		{
			bAnyCanceled = bAnyCanceled
				|| Outcome.WaitStatus != ETaskWaitStatus::Completed
				|| Outcome.TaskState == ETaskState::Canceled
				|| Outcome.TaskState == ETaskState::Invalid;
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

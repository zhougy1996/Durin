#include "Threading/Task.h"
#include "Threading/TaskPipe.h"
#include "Threading/TaskConcurrencyLimiter.h"
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
			CancellationRequested,
			DispatchRejected,
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
				Out.CancellationRequestedCount = Counter(ETaskAggregateCounter::CancellationRequested);
				Out.DispatchRejectedCount = Counter(ETaskAggregateCounter::DispatchRejected);
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
			ETaskState ExecutionState = ETaskState::Invalid;
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

		auto RequestCancellation() -> void { bCancellationRequested.store(true, std::memory_order_release); }

	private:
		std::atomic<bool> bCancellationRequested = false;
	};



	class FTaskGenerationState
	{
	public:
		std::atomic<uint64> Generation = 1;
	};

	enum class ETaskDependencyKind : uint8 { Execution, Nested, Dynamic, Pipe };

	// Per-walk scratch, never a graph cache. Open addressing retains allocations
	// across wakeups instead of allocating a hash node for every visited task.
	class FTaskGraphVisited
	{
		struct FSlot { uint64 Id = 0; uint8 Flags = 0; };
		std::array<FSlot, 64> Inline{};
		std::vector<FSlot> Overflow;
		size_t Count = 0;
		auto Slots() -> std::span<FSlot> { return Overflow.empty() ? std::span<FSlot>(Inline) : std::span<FSlot>(Overflow); }
		static auto Find(std::span<FSlot> Table, uint64 Id) -> FSlot&
		{
			size_t Index = static_cast<size_t>(((Id ^ (Id >> 33)) * 0xff51afd7ed558ccdull) >> 32) & (Table.size() - 1);
			while (Table[Index].Id != 0 && Table[Index].Id != Id) Index = (Index + 1) & (Table.size() - 1);
			return Table[Index];
		}
	public:
		auto Visit(uint64 Id, uint8 Flags = 1) -> bool
		{
			if ((Count + 1) * 2 > Slots().size())
			{
				std::vector<FSlot> Grown(Slots().size() * 2);
				for (const auto& Slot : Slots()) if (Slot.Id) Find(Grown, Slot.Id) = Slot;
				Overflow = std::move(Grown);
			}
			auto& Slot = Find(Slots(), Id);
			if ((Slot.Flags & Flags) == Flags) return false;
			if (!Slot.Id) { Slot.Id = Id; ++Count; }
			Slot.Flags |= Flags;
			return true;
		}
		auto Reset() -> void { std::ranges::fill(Slots(), FSlot{}); Count = 0; }
	};

	// Execution edges occupy a fixed prefix. Nested and the optional dynamic edge
	// share the suffix; weak backlinks survive consumption of their owning refs.
	// The owning node's mutex protects every operation on this collection.
	class FTaskDependencyEdges
	{
	public:
		enum class ERelease : uint8 { ExecutionStarted, RetractionAttempt, Closed };
		auto Reserve(size_t Count) -> void { Edges.reserve(Count); }
		auto ReserveAdditional() -> void
		{
			if (Edges.size() == Edges.capacity())
				Edges.reserve(Edges.size() + std::max(size_t{1}, Edges.size() / 2));
		}
		auto Add(const std::shared_ptr<FTaskStateData>& Node, uint64 TaskId, ETaskDependencyKind Kind) -> void
		{
			if (Kind == ETaskDependencyKind::Execution) check(Edges.size() == ExecutionCount);
			if (Kind == ETaskDependencyKind::Dynamic) check(DynamicIndex == NoEdge);
			const size_t Index = Edges.size();
			Edges.push_back({Node, Node, TaskId});
			if (Kind == ETaskDependencyKind::Execution) ++ExecutionCount;
			if (Kind == ETaskDependencyKind::Dynamic) DynamicIndex = Index;
		}
		auto TakeExecutionPin(size_t Index) -> std::shared_ptr<FTaskStateData>
		{ check(Index < ExecutionCount); return std::move(Edges[Index].Lifetime); }
		template<typename F>
		auto Visit(F&& Function, bool bExecution = true, bool bNested = true, bool bDynamic = true) const -> void
		{
			auto Pin = [&](size_t Index) {
				if (auto Node = Edges[Index].Backlink.lock()) Function(std::move(Node));
			};
			if (bDynamic && DynamicIndex != NoEdge) Pin(DynamicIndex);
			if (bNested)
				for (size_t Index = ExecutionCount; Index < Edges.size(); ++Index)
					if (Index != DynamicIndex) Pin(Index);
			if (bExecution)
				for (size_t Index = 0; Index < ExecutionCount; ++Index) Pin(Index);
		}
		auto AppendTo(std::vector<std::shared_ptr<FTaskStateData>>& Out, bool bExecution = true, bool bNested = true) const -> void
		{ Visit([&](auto Node) { Out.push_back(std::move(Node)); }, bExecution, bNested); }
		auto Release(ERelease Phase) -> std::vector<std::shared_ptr<FTaskStateData>>
		{
			auto Select = [&](size_t Index) {
				return Phase == ERelease::Closed || (Index != DynamicIndex
					&& (Phase == ERelease::RetractionAttempt || Index < ExecutionCount));
			};
			size_t Count = 0;
			for (size_t Index = 0; Index < Edges.size(); ++Index)
				if (Select(Index) && Edges[Index].Lifetime) ++Count;
			std::vector<std::shared_ptr<FTaskStateData>> Released;
			Released.reserve(Count);
			for (size_t Index = 0; Index < Edges.size(); ++Index)
				if (Select(Index) && Edges[Index].Lifetime) Released.push_back(std::move(Edges[Index].Lifetime));
			if (Phase == ERelease::Closed)
			{
				Edges.resize(ExecutionCount);
				DynamicIndex = NoEdge;
			}
			return Released; // The caller destroys refs after releasing the node lock.
		}
		auto GetExecutionTaskIds() const -> std::vector<uint64>
		{
			std::vector<uint64> Ids;
			Ids.reserve(ExecutionCount);
			for (size_t Index = 0; Index < ExecutionCount; ++Index) Ids.push_back(Edges[Index].TaskId);
			return Ids;
		}
		auto GetStorageBytes() const -> uint64 { return Edges.capacity() * sizeof(FEdge); }
	private:
		struct FEdge
		{
			std::weak_ptr<FTaskStateData> Backlink;
			std::shared_ptr<FTaskStateData> Lifetime;
			uint64 TaskId = 0;
		};
		static constexpr size_t NoEdge = std::numeric_limits<size_t>::max();
		std::vector<FEdge> Edges;
		size_t ExecutionCount = 0;
		size_t DynamicIndex = NoEdge;
	};

	struct FTaskSubsequent
	{
		std::weak_ptr<FTaskStateData> Node;
		ETaskDependencyKind Kind;
	};

	enum class ETaskNodeKind : uint8 { Executable, External, Event };

	// A serial closes the gap between inspecting graph progress and sleeping.
	class FTaskWaitSignal
	{
	public:
		auto Notify() -> void
		{
			{ std::lock_guard Lock(Mutex); ++Serial; }
			CV.notify_all();
		}
		auto PrepareWait() -> uint64
		{ std::lock_guard Lock(Mutex); return Serial; }
		auto WaitFor(uint64 Observed, double TimeoutSeconds) -> void
		{
			std::unique_lock Lock(Mutex);
			auto Changed = [&] { return Serial != Observed; };
			if (std::isinf(TimeoutSeconds)) CV.wait(Lock, Changed);
			else CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), Changed);
		}
	private:
		std::mutex Mutex;
		std::condition_variable CV;
		uint64 Serial = 0;
	};

	struct Tasks::Private::FTaskConcurrencyLimiterState
	{
		std::mutex Mutex;
		std::weak_ptr<FTaskWaitSignal> WaitSignal;
		std::deque<std::weak_ptr<FTaskStateData>> Pending;
		uint64 TaskCount = 0;
		std::vector<uint32> FreeSlots;
		std::vector<std::weak_ptr<FTaskStateData>> Scheduled;
		ETaskPriority Priority;
		auto AddTask() -> void { std::lock_guard Lock(Mutex); ++TaskCount; }
		auto RemoveTask(uint32 Slot) -> void;
	};

	struct FTaskNodeInit
	{
		Tasks::FPipe* Pipe = nullptr;
		std::shared_ptr<Tasks::Private::FTaskConcurrencyLimiterState> ConcurrencyLimiter;
		const char* DebugName = "Task";
		std::weak_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting;
		FTaskCancellationToken CancellationToken;
		std::vector<std::shared_ptr<FTaskStateData>> Prerequisites;
		uint64 ParentTaskId = 0;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		uint64 EstimatedPayloadBytes = 0;
		uint64 AdditionalStorageBytes = 0;
		bool bIndependentCPU = false;
		FTaskAttribution Attribution;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		ETaskNodeKind Kind = ETaskNodeKind::Executable;
		bool bUnknownExecutionRequirement = false;
		Private::FMoveOnlyTaskFunction Function;
		Private::FTaskCompletionFunction Completion;
		Private::FTaskResultStorage Result;
	};

	// Owns one scheduler task node independently of public handles and pool work items.
	class FTaskStateData final : public Private::FQueuedWorkNode, public std::enable_shared_from_this<FTaskStateData>
	{
	public:
		explicit FTaskStateData(FTaskNodeInit& Init)
			: TaskId(GNextTaskId.fetch_add(1, std::memory_order::acq_rel))
			, ParentTaskId(Init.ParentTaskId)
			, DebugName(Init.DebugName ? Init.DebugName : "Task")
			, Scheduler(Init.Scheduler)
			, LifetimeAccounting(Init.LifetimeAccounting)
			, SharedCancellationState(Init.CancellationToken.SharedState)
			, bHasResultStorage(Init.Result.bHasValue)
			, bConstructionPending(Init.Kind == ETaskNodeKind::Executable || Init.Kind == ETaskNodeKind::External)
			, Kind(Init.Kind)
			, Event(Init.Kind == ETaskNodeKind::Event ? std::make_unique<FEventData>() : nullptr)
			, bUnknownExecutionRequirement(Init.bUnknownExecutionRequirement)
			, RemainingPrerequisites(static_cast<uint32>(Init.Prerequisites.size()))
			, ExecutionState(!bConstructionPending ? ETaskState::Running : Init.Prerequisites.empty() ? ETaskState::Queued : ETaskState::Waiting)
			, Target(Init.Target)
			, Priority(Init.Priority)
			, EstimatedPayloadBytes(Init.EstimatedPayloadBytes)
			, bIndependentCPU(Init.bIndependentCPU)
			, Attribution(Init.Attribution)
			, GenerationToken(Init.GenerationToken)
			, CoalescingKey(Init.CoalescingKey)
			, EnqueueTimeNanoseconds(MonotonicNanoseconds())
		{
			require(LifetimeAccounting && (!Init.Result.bHasValue || Init.Result.Storage));
			Dependencies.Reserve(Init.Prerequisites.size());
			for (const std::shared_ptr<FTaskStateData>& Prerequisite : Init.Prerequisites)
			{
				Dependencies.Add(Prerequisite, Prerequisite->GetTaskId(), ETaskDependencyKind::Execution);
			}
			if (bConstructionPending) SchedulerStorageBytes = sizeof(FTaskStateData)
				+ Dependencies.GetStorageBytes() + Init.AdditionalStorageBytes;
			// All throwing construction precedes payload transfer. On failure Init
			// still owns captures and is destroyed outside scheduler synchronization.
			PendingFunction = std::move(Init.Function);
			CompletionFunction = std::move(Init.Completion);
			ResultStorage = std::move(Init.Result.Storage);
			Pipe = Init.Pipe;
			bPipeMayHaveDependents = !Init.Prerequisites.empty();
			if (Pipe) { Pipe->AddTask(); bPipeCounted = true; }
			ConcurrencyLimiter = std::move(Init.ConcurrencyLimiter);
			if (ConcurrencyLimiter) ConcurrencyLimiter->AddTask();
		}

		~FTaskStateData()
		{
			if (bPipeCounted) Pipe->RemoveTask();
			if (ConcurrencyLimiter) ConcurrencyLimiter->RemoveTask(ConcurrencySlot);
			requiref(!Event || (Phase == EPhase::Closed),
				"Task event nodes must be completed before their final reference is released.");
			ResultStorage.reset();
			ResultLifetimes.clear();
			if (Phase != EPhase::Closed)
			{
				return;
			}
			const uint64 PreviousTaskCount = LifetimeAccounting->RetainedTerminalTaskCount.fetch_sub(1, std::memory_order::acq_rel);
			require(PreviousTaskCount > 0);
			if (bHasResultStorage)
			{
				const uint64 PreviousResultCount = LifetimeAccounting->RetainedTerminalResultCount.fetch_sub(1, std::memory_order::acq_rel);
				require(PreviousResultCount > 0);
			}
		}
		auto RetainLifetime(std::shared_ptr<void> Lifetime) -> void
		{
			std::lock_guard Lock(Mutex);
			ResultLifetimes.push_back(std::move(Lifetime));
		}

		auto SetFailure(Tasks::FTaskFailure Failure) -> void
		try
		{
			auto Storage = std::make_unique<Tasks::FTaskFailure>(std::move(Failure));
			std::lock_guard Lock(Mutex);
			require(!FailureStorage);
			FailureStorage = std::move(Storage);
		}
		catch (const std::bad_alloc&) { Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::AllocationFailure}); }
		auto GetFailure() const -> Tasks::FTaskFailure
		{
			std::lock_guard Lock(Mutex);
			return FailureStorage ? *FailureStorage : Tasks::FTaskFailure{Outcome.Reason};
		}

		auto Activate() -> void
		{
			std::lock_guard Lock(Mutex);
			require(bConstructionPending);
			bConstructionPending = false;
		}
		auto ReserveDependent() -> void
		{
			std::lock_guard Lock(Mutex);
			ReserveSubsequentLocked();
			++ReservedDependents;
		}
		auto ReleaseDependentReservation() -> void
		{
			std::lock_guard Lock(Mutex);
			require(ReservedDependents > 0);
			--ReservedDependents;
		}

		auto IsRunningBody() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return ExecutionState == ETaskState::Running && !IsExternal() && Phase == EPhase::Open;
		}
		auto IsExternal() const -> bool { return Kind != ETaskNodeKind::Executable; }
		auto IsTaskEvent() const -> bool { return Event != nullptr; }
		auto BindTerminal(std::shared_ptr<Private::FTaskTerminalHook> Hook) -> void
		{
			ETaskState Terminal;
			{
				std::lock_guard Lock(Mutex);
				if (Phase != EPhase::Closed)
				{
					Hook->Next = std::move(TerminalHooks);
					TerminalHooks = std::move(Hook);
					return;
				}
				Terminal = Outcome.State;
			}
			Hook->Function(Terminal);
		}
		auto SetDynamicDependency(std::shared_ptr<FTaskStateData> Inner) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (bDynamicDependencyBound) return false;
			if (Phase < EPhase::Closing) Dependencies.Add(Inner, Inner->GetTaskId(), ETaskDependencyKind::Dynamic);
			bDynamicDependencyBound = true;
			bUnknownExecutionRequirement = false;
			return true;
		}
		// Ordinary waits own one signal; concurrent limiter waits share one.
		// Graph subscriptions are weak and expire when the interested waits return.
		using FWaitSignal = FTaskWaitSignal;
		auto RegisterWaiter(const std::shared_ptr<FWaitSignal>& Signal) -> void
		{
			std::lock_guard Lock(Mutex);
			std::erase_if(Waiters, [](const auto& Weak) { return Weak.expired(); });
			for (const auto& Weak : Waiters) if (Weak.lock() == Signal) return;
			Waiters.emplace_back(Signal);
		}
		auto NotifyStateChanged() -> void
		{
			std::lock_guard Lock(Mutex);
			std::erase_if(Waiters, [](const auto& Weak) {
				if (auto Signal = Weak.lock()) { Signal->Notify(); return false; }
				return true;
			});
		}

		auto ReserveSubsequentLocked() -> void
		{
			const size_t Required = Subsequents.size() + ReservedDependents + 1;
			if (Required > Subsequents.capacity())
				Subsequents.reserve(std::max(Required, Subsequents.capacity() + Subsequents.capacity() / 2));
		}
		auto RegisterDependentLocked(const std::shared_ptr<FTaskStateData>& Dependent, ETaskDependencyKind Kind) -> ETaskState
		{
			if (Phase == EPhase::Closed) return Outcome.State;
			Subsequents.push_back({Dependent, Kind});
			return ExecutionState;
		}
		auto RegisterDependent(const std::shared_ptr<FTaskStateData>& Dependent) -> ETaskState
		{
			std::lock_guard Lock(Mutex);
			require(ReservedDependents > 0);
			--ReservedDependents;
			return RegisterDependentLocked(Dependent, ETaskDependencyKind::Execution);
		}

		auto OnPrerequisiteTerminal() -> void;
		// The running body or closing phase exclusively owns the callback. Never
		// move or destroy user captures under the node mutex.
		auto InvokeBody(const FTaskCancellationToken& Token) -> void { PendingFunction(Token); }
		auto ReleaseBody() -> void { PendingFunction = nullptr; }

		auto PrepareForQueue() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (Phase != EPhase::Open || ExecutionState != ETaskState::Queued || bConstructionPending || bQueueClaimed || bPipeWaiting)
			{
				return false;
			}
			bQueueClaimed = true;
			if (!Pipe && !ConcurrencyLimiter) DispatchTimeNanoseconds = MonotonicNanoseconds();
			return true;
		}

		auto EnterPipe() -> bool
		{
			if (!Pipe || bPiped) return true;
			// Serialize tail and graph mutation with nested/dynamic edge insertion.
			// The pipe's count mutex is separate and is never taken here.
			std::lock_guard GraphLock(GTaskDependencyMutex);
			auto Previous = Pipe->LastTask.lock();
			bPiped = true;
			if (!Previous) { Pipe->LastTask = shared_from_this(); return true; }
			// A newly launched ready task has not escaped admission yet. Only a
			// task delayed by prerequisites can already have incoming graph edges.
			if (bPipeMayHaveDependents && !IsTerminalState(Previous->GetState()))
				requiref(!Previous->DependsOn(this), "Pipe ordering would create a task dependency cycle.");
			std::scoped_lock Lock(Mutex, Previous->Mutex);
			if (Previous->Phase == EPhase::Closed) { Pipe->LastTask = shared_from_this(); return true; }
			Dependencies.ReserveAdditional();
			Previous->ReserveSubsequentLocked();
			Dependencies.Add(Previous, Previous->GetTaskId(), ETaskDependencyKind::Execution);
			Previous->RegisterDependentLocked(shared_from_this(), ETaskDependencyKind::Pipe);
			bPipeWaiting = true;
			// Publish only after all throwing preparation.
			Pipe->LastTask = shared_from_this();
			return false;
		}
		auto EnterConcurrencyLimiter() -> bool
		{
			if (!ConcurrencyLimiter || ConcurrencySlot != std::numeric_limits<uint32>::max()) return true;
			std::lock_guard Lock(ConcurrencyLimiter->Mutex);
			if (!ConcurrencyLimiter->FreeSlots.empty())
			{
				ConcurrencySlot = ConcurrencyLimiter->FreeSlots.back();
				ConcurrencyLimiter->FreeSlots.pop_back();
				ConcurrencyLimiter->Scheduled[ConcurrencySlot] = weak_from_this();
				if (auto Signal = ConcurrencyLimiter->WaitSignal.lock()) Signal->Notify();
				return true;
			}
			ConcurrencyLimiter->Pending.push_back(weak_from_this());
			return false;
		}
		auto OnConcurrencyPermit(uint32 Slot) -> void;
		auto GetConcurrencySlot() const -> uint32 { return ConcurrencySlot; }
		auto GetConcurrencyLimiter() const -> Tasks::Private::FTaskConcurrencyLimiterState* { return ConcurrencyLimiter.get(); }
		auto CompleteConcurrencyWork() -> std::shared_ptr<Tasks::Private::FTaskConcurrencyLimiterState>
		{
			auto Limiter = std::move(ConcurrencyLimiter);
			if (Limiter) Limiter->RemoveTask(ConcurrencySlot);
			return Limiter;
		}
		auto OnPipePredecessorTerminal() -> void;
		auto GetPipe() const -> Tasks::FPipe* { return Pipe; }
		auto MarkDispatched() -> void
		{ std::lock_guard Lock(Mutex); DispatchTimeNanoseconds = MonotonicNanoseconds(); }

		auto Execute() -> void override;
		auto Discard() -> void override
		{ FinishBody(ETaskState::Failed, "Task executor discarded accepted work.", ETaskTerminalReason::DispatchRejected); }
		auto GetName() const -> const char* override { return GetDebugName(); }

		auto TryMarkRunning() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (Phase != EPhase::Open || ExecutionState != ETaskState::Queued)
			{
				return false;
			}
			// Execution and cancellation claim the callback under the same lock.
			ExecutingThreadName = GetCurrentThreadName();
			ExecutionState = ETaskState::Running;
			StartTimeNanoseconds = MonotonicNanoseconds();
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
			return bCancellationRequested.load(std::memory_order_acquire)
				|| (SharedCancellationState && SharedCancellationState->IsCancellationRequested());
		}

		auto DependsOn(const FTaskStateData* PotentialPrerequisite) const -> bool
		{
			std::vector<std::shared_ptr<FTaskStateData>> PendingPrerequisites;
			{
				std::lock_guard Lock(Mutex);
				Dependencies.AppendTo(PendingPrerequisites);
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
				Prerequisite->Dependencies.AppendTo(PendingPrerequisites);
			}
			return false;
		}

		// Inspect one node lock at a time. Terminal ancestors impose no executor
		// requirement; pinning each frontier keeps concurrent publication safe.
		auto IsIndependentCPU() const -> bool { return bIndependentCPU; }

		struct FWaitGraphWork
		{
			struct FEntry { std::shared_ptr<const FTaskStateData> Node; bool bRequirements; };
			std::vector<FEntry> Pending;
			FTaskGraphVisited Visited;
		};
		struct FWaitRequirements { bool bCycle = false; bool bUnknown = false; bool bGameThread = false; };
		auto InspectWaitGraph(const FTaskStateData* CycleTarget, FWaitGraphWork& Work) const -> FWaitRequirements
		{
			FWaitRequirements Result;
			Work.Pending.push_back({shared_from_this(), true});
			while (!Work.Pending.empty())
			{
				auto Entry = std::move(Work.Pending.back());
				Work.Pending.pop_back();
				if (Entry.Node.get() == CycleTarget) { Result.bCycle = true; break; }
				if (!Work.Visited.Visit(Entry.Node->GetTaskId(), Entry.bRequirements ? 3 : 1)) continue;
				std::lock_guard Lock(Entry.Node->Mutex);
				const bool bRequirements = Entry.bRequirements && Entry.Node->Phase != EPhase::Closed;
				if (bRequirements)
				{
					Result.bUnknown |= Entry.Node->bUnknownExecutionRequirement;
					Result.bGameThread |= Entry.Node->Phase == EPhase::Open && Entry.Node->Target == ETaskTarget::GameThreadDeferred;
				}
				// Closed ancestors still participate in cycle identity, but must not
				// leak their former executor restrictions into an active wait.
				if (CycleTarget || bRequirements)
					Entry.Node->Dependencies.Visit([&](auto Node) { Work.Pending.push_back({std::move(Node), bRequirements}); });
			}
			Work.Pending.clear();
			Work.Visited.Reset();
			return Result;
		}

		auto AppendRetractionDependencies(std::vector<std::shared_ptr<FTaskStateData>>& Result) -> void
		{
			std::vector<std::shared_ptr<FTaskStateData>> Released;
			{
				std::lock_guard Lock(Mutex);
				if (Phase == EPhase::Closed) return;
				Dependencies.AppendTo(Result, RemainingPrerequisites != 0 || bPipeWaiting, PendingNestedCount != 0);
				Released = Dependencies.Release(FTaskDependencyEdges::ERelease::RetractionAttempt);
			}
		}
		auto ReleaseCompletedPrerequisite(size_t Index) -> void
		{
			std::shared_ptr<FTaskStateData> Released;
			{ std::lock_guard Lock(Mutex); Released = Dependencies.TakeExecutionPin(Index); }
		}
		auto ReleaseExecutionDependencies() -> void
		{
			std::vector<std::shared_ptr<FTaskStateData>> Released;
			{ std::lock_guard Lock(Mutex); Released = Dependencies.Release(FTaskDependencyEdges::ERelease::ExecutionStarted); }
		}
		auto RequestCancellation() -> bool;
		auto SetCanceledOutcome() -> void
		{
			std::lock_guard Lock(Mutex);
			require(Phase == EPhase::Open && ExecutionState == ETaskState::Running);
			Outcome = {ETaskState::Canceled, "Task producer reported a canceled operation.", ETaskTerminalReason::CancellationRequested};
		}
		auto OnNestedCompleted() -> void;
		auto FinishBody(ETaskState TerminalState, std::string InDiagnostic = {}, ETaskTerminalReason InReason = ETaskTerminalReason::None) -> bool;

		auto GetState() const -> ETaskState
		{
			std::lock_guard Lock(Mutex);
			return Phase == EPhase::Closed ? Outcome.State : ExecutionState;
		}

		auto GetDiagnostic() const -> std::string
		{
			std::lock_guard Lock(Mutex);
			return Phase == EPhase::Closed ? Outcome.Diagnostic : std::string{};
		}

		auto GetDiagnostics() const -> FTaskDiagnostics
		{
			FTaskDiagnostics Snapshot;
			{
				std::lock_guard Lock(Mutex);
				Snapshot.TaskId = TaskId;
				Snapshot.ParentTaskId = ParentTaskId;
				Snapshot.PrerequisiteTaskIds = Dependencies.GetExecutionTaskIds();
				const bool bClosed = Phase == EPhase::Closed;
				Snapshot.EnqueueTimeNanoseconds = EnqueueTimeNanoseconds;
				Snapshot.DispatchTimeNanoseconds = DispatchTimeNanoseconds;
				Snapshot.QueueResidencyNanoseconds = DispatchTimeNanoseconds == 0
					? 0
					: (StartTimeNanoseconds == 0 ? MonotonicNanoseconds() : StartTimeNanoseconds) - DispatchTimeNanoseconds;
				Snapshot.StartTimeNanoseconds = StartTimeNanoseconds;
				Snapshot.FinishTimeNanoseconds = bClosed ? FinishTimeNanoseconds : 0;
				Snapshot.ExecutionNanoseconds = StartTimeNanoseconds == 0
					? 0
					: (!bClosed || FinishTimeNanoseconds == 0 ? MonotonicNanoseconds() : FinishTimeNanoseconds) - StartTimeNanoseconds;
				Snapshot.ExecutingThreadId = ExecutingThreadId;
				Snapshot.DebugName = DebugName;
				Snapshot.ExecutingThreadName = ExecutingThreadName;
				Snapshot.Diagnostic = bClosed ? Outcome.Diagnostic : std::string{};
				Snapshot.State = bClosed ? Outcome.State : ExecutionState;
				Snapshot.Target = Target;
				Snapshot.Priority = Priority;
				Snapshot.TerminalReason = bClosed ? Outcome.Reason : ETaskTerminalReason::None;
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
				Snapshot.bHasResultStorage = bHasResultStorage;
			}
			std::tie(Snapshot.AttributionOwner, Snapshot.AttributionCategory) = GTaskAttributionRegistry.Resolve(Attribution);
			return Snapshot;
		}

		auto GetAccountingSnapshot() const -> FTaskAccountingSnapshot
		{
			std::lock_guard Lock(Mutex);
			return {
				.Attribution = Attribution,
				.State = Phase == EPhase::Closed ? Outcome.State : ExecutionState,
				.ExecutionState = ExecutionState,
				.TerminalReason = Outcome.Reason,
				.PayloadBytes = EstimatedPayloadBytes,
				.QueueResidencyNanoseconds = DispatchTimeNanoseconds == 0 || StartTimeNanoseconds == 0 ? 0 : StartTimeNanoseconds - DispatchTimeNanoseconds,
				.ExecutionNanoseconds = StartTimeNanoseconds == 0 || FinishTimeNanoseconds == 0 ? 0 : FinishTimeNanoseconds - StartTimeNanoseconds,
			};
		}

		auto GetDebugName() const -> const char* { return DebugName.c_str(); }
		auto GetTaskId() const -> uint64 { return TaskId; }
		auto PinScheduler() const -> std::shared_ptr<FTaskScheduler> { return Scheduler.lock(); }
		auto GetTarget() const -> ETaskTarget { return Target; }
		auto GetPriority() const -> ETaskPriority { return Priority; }
		auto GetAttribution() const -> FTaskAttribution { return Attribution; }
		auto GetEstimatedPayloadBytes() const -> uint64 { return EstimatedPayloadBytes; }
		auto GetSchedulerStorageBytes() const -> uint64 { return SchedulerStorageBytes; }
		auto GetGenerationToken() const -> const FTaskGenerationToken& { return GenerationToken; }
		auto GetCoalescingKey() const -> const std::optional<FTaskCoalescingKey>& { return CoalescingKey; }

	private:
		enum class EPhase : uint8 { Open, BodyFinished, Closing, Closed };
		struct FTaskOutcome
		{
			ETaskState State = ETaskState::Invalid;
			std::string Diagnostic;
			ETaskTerminalReason Reason = ETaskTerminalReason::None;
		};
		EPhase Phase = EPhase::Open;
		FTaskOutcome Outcome;
		// Detailed composition failures are absent on the successful path.
		std::unique_ptr<Tasks::FTaskFailure> FailureStorage;
		auto TryClose() -> void;
		auto Close() noexcept -> void;
		friend struct Private::FTaskRuntimeAccess;

		uint64 TaskId = 0;
		uint64 ParentTaskId = 0;
		std::string DebugName;
		std::weak_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting;
		std::shared_ptr<FTaskCancellationState> SharedCancellationState;
		mutable std::mutex Mutex;
		std::vector<std::weak_ptr<FWaitSignal>> Waiters;
		Private::FMoveOnlyTaskFunction PendingFunction;
		Private::FTaskCompletionFunction CompletionFunction;
		std::shared_ptr<void> ResultStorage;
		std::vector<std::shared_ptr<void>> ResultLifetimes;
		bool bHasResultStorage = false;
		bool bConstructionPending = true;
		Tasks::FPipe* Pipe = nullptr;
		std::shared_ptr<Tasks::Private::FTaskConcurrencyLimiterState> ConcurrencyLimiter;
		bool bPipeCounted = false;
		uint32 ConcurrencySlot = std::numeric_limits<uint32>::max();
		bool bPiped = false;
		bool bPipeMayHaveDependents = false;
		bool bPipeWaiting = false;
		bool bQueueClaimed = false;
		size_t ReservedDependents = 0;
		const ETaskNodeKind Kind;
		struct FEventData
		{
			std::mutex SignalMutex;
			bool Triggered = false;
			// Triggered events retain pending completion independently of a scheduler.
			std::shared_ptr<FTaskStateData> CompletionLifetime;
		};
		std::unique_ptr<FEventData> Event;
		uint32 PendingNestedCount = 0;
		bool bUnknownExecutionRequirement = false;
		std::shared_ptr<Private::FTaskTerminalHook> TerminalHooks;
		bool bDynamicDependencyBound = false;
		FTaskDependencyEdges Dependencies;
		std::vector<FTaskSubsequent> Subsequents;
		uint32 RemainingPrerequisites = 0;
		// Execution progress remains nonterminal; only Closed exposes Outcome.State.
		ETaskState ExecutionState = ETaskState::Queued;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		uint64 EstimatedPayloadBytes = 0;
		const bool bIndependentCPU;
		uint64 SchedulerStorageBytes = 0;
		FTaskAttribution Attribution;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		std::atomic<bool> bCancellationRequested = false;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 DispatchTimeNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string ExecutingThreadName;
	};

	auto DispatchGameThreadDeferredTask(const std::shared_ptr<FTaskStateData>& State) -> bool;
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
			Scheduler->BlockingIOWarningThreshold = Config.BlockingIOWarningThreshold;
			Scheduler->WorkerCount = NumThreads;
			Scheduler->TaskWarningThreshold = Config.TaskWarningThreshold;
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
			Private::FMoveOnlyTaskFunction& FunctionOwner,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			ETaskTarget Target = ETaskTarget::AnyWorker,
			ETaskPriority Priority = ETaskPriority::Normal,
			uint64 EstimatedPayloadBytes = 0,
			uint64 AdditionalStorageBytes = 0,
			FTaskGenerationToken GenerationToken = {},
			std::optional<FTaskCoalescingKey> CoalescingKey = {},
			Private::FTaskResultStorage Result = {}, const Tasks::FTask* PrimaryPrerequisite = nullptr) -> std::shared_ptr<FTaskStateData>
		{
			FTaskNodeInit Init;
			Init.Pipe = Options.Pipe;
			Init.ConcurrencyLimiter = Options.ConcurrencyLimiter;
			Init.DebugName = Name;
			Init.Scheduler = weak_from_this();
			Init.LifetimeAccounting = LifetimeAccounting;
			Init.CancellationToken = Options.CancellationToken;
			Init.ParentTaskId = GCurrentTaskState ? GCurrentTaskState->GetTaskId() : 0;
			Init.Target = Target;
			Init.Priority = Priority;
			Init.EstimatedPayloadBytes = EstimatedPayloadBytes;
			Init.AdditionalStorageBytes = AdditionalStorageBytes;
			Init.bIndependentCPU = Options.bIndependentCPU;
			Init.Attribution = Options.Attribution;
			Init.GenerationToken = std::move(GenerationToken);
			Init.CoalescingKey = std::move(CoalescingKey);
			Init.Kind = Options.bExternalCompletion ? ETaskNodeKind::External : ETaskNodeKind::Executable;
			Init.bUnknownExecutionRequirement = Options.bExternalCompletion && Options.bUnknownExecutionRequirement;
			Init.Function = std::move(FunctionOwner);
			Init.Completion = std::move(CompletionFunction);
			Init.Result = std::move(Result);
			std::shared_ptr<FTaskStateData> State;
			size_t ReservedDependencyCount = 0;
			auto& PrerequisiteStates = Init.Prerequisites;
			// Preparation owns no scheduler reservation. The submission guard keeps
			// shutdown from retiring this scheduler until preparation/rollback ends.
			struct FDependencyReservations
			{
				std::vector<std::shared_ptr<FTaskStateData>>& Prerequisites;
				size_t& Count;
				~FDependencyReservations()
				{
					for (size_t Index = 0; Index < Count; ++Index) Prerequisites[Index]->ReleaseDependentReservation();
				}
			} Reservations{PrerequisiteStates, ReservedDependencyCount};
			if ((GCurrentTaskState && GCurrentTaskState->IsIndependentCPU())
				|| (Options.ConcurrencyLimiter && (Options.Pipe || Target != ETaskTarget::AnyWorker || Options.bExternalCompletion || Options.bIndependentCPU))
				|| (Options.Pipe && (Target != ETaskTarget::AnyWorker || Options.bExternalCompletion || Options.bIndependentCPU))
				|| (Options.bIndependentCPU && (Target != ETaskTarget::AnyWorker
					|| Options.bExternalCompletion || PrimaryPrerequisite || std::ranges::any_of(Options.Prerequisites, [](const auto& Task) { return Task.IsValid(); }))))
				Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::InvalidExecutionContract});
			PrerequisiteStates.reserve(Options.Prerequisites.size() + (PrimaryPrerequisite ? 1 : 0));
			auto AppendPrerequisite = [&](const Tasks::FTask& Prerequisite) {
				if (!Prerequisite.State) return;
				if (PrimaryPrerequisite && std::ranges::find(PrerequisiteStates, Prerequisite.State) != PrerequisiteStates.end()) return;
				if (!Prerequisite.State->IsTaskEvent() && Prerequisite.State->PinScheduler().get() != this)
				{
					Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::InvalidPrerequisite, Prerequisite.GetTaskId()});
				}
				PrerequisiteStates.emplace_back(Prerequisite.State);
			};
			if (PrimaryPrerequisite) AppendPrerequisite(*PrimaryPrerequisite);
			for (const Tasks::FTask& Prerequisite : Options.Prerequisites) AppendPrerequisite(Prerequisite);
			State = std::make_shared<FTaskStateData>(Init);
			for (const auto& Prerequisite : PrerequisiteStates)
			{
				Prerequisite->ReserveDependent();
				++ReservedDependencyCount;
			}
			bool bReportOverload = false;
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingTasks)
				{
					Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::LifetimeClosed});
				}

				++CurrentTrackedTaskCount;
				PeakTrackedTaskCount = std::max(PeakTrackedTaskCount, CurrentTrackedTaskCount);

				try
				{
					ActiveTasks.emplace(State->GetTaskId(), State);
					if (Target == ETaskTarget::BlockingIO) ++BlockingIOTrackedTasks;
				}
				catch (...)
				{
					require(CurrentTrackedTaskCount > 0);
					--CurrentTrackedTaskCount;
					ActiveTasks.erase(State->GetTaskId());
					throw;
				}
				auto& Storage = ExecutorStorage[static_cast<size_t>(Target)];
				++Storage.CurrentNodes;
				++Storage.CurrentPendingNodes;
				Storage.CurrentPendingBytes += State->GetSchedulerStorageBytes();
				Storage.PeakPendingNodes = std::max(Storage.PeakPendingNodes, Storage.CurrentPendingNodes);
				Storage.PeakPendingBytes = std::max(Storage.PeakPendingBytes, Storage.CurrentPendingBytes);
				Storage.CurrentBytes += State->GetSchedulerStorageBytes();
				Storage.PeakNodes = std::max(Storage.PeakNodes, Storage.CurrentNodes);
				Storage.PeakBytes = std::max(Storage.PeakBytes, Storage.CurrentBytes);
				const uint64 Threshold = Target == ETaskTarget::BlockingIO ? BlockingIOWarningThreshold : TaskWarningThreshold;
				if (Storage.CurrentNodes == Threshold + 1)
				{
					const uint64 Crossings = ++Storage.OverloadCrossings;
					bReportOverload = (Crossings & (Crossings - 1)) == 0;
				}
				RecordAcceptedTask(State->GetAccountingSnapshot());
				// Committed registrations now own the reserved dependent slots.
				ReservedDependencyCount = 0;
			}

			// Registry publication commits lifetime tracking before activation.
			size_t RegisteredDependencies = 0;
			try
			{
				if (bReportOverload) DURIN_WARN("Task executor {} crossed its storage threshold; valid work remains queued.", static_cast<uint32>(Target));
				if (State->IsExternal())
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
						State->ReleaseCompletedPrerequisite(RegisteredDependencies - 1);
						State->OnPrerequisiteTerminal();
					}
				}
			}
			catch (const std::bad_alloc&)
			{
				requiref(false, "Task registration allocation failed.");
				std::terminate();
			}
			State->Activate();
			if (!State->IsExternal()) QueueTask(State);

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
				if (!State->EnterPipe()) { State->NotifyStateChanged(); return; }
				if (!State->EnterConcurrencyLimiter()) { State->NotifyStateChanged(); return; }
				if (State->GetPipe() || State->GetConcurrencyLimiter()) State->MarkDispatched();
				if (State->GetTarget() == ETaskTarget::GameThreadDeferred)
				{
					if (!DispatchGameThreadDeferredTask(State))
					{
						requiref(IsTerminalState(State->GetState()), "Accepted task dispatch failed.");
					RecordRejectedTask(State->GetAttribution());
						State->FinishBody(ETaskState::Failed, "GameThread deferred executor rejected task dispatch.", ETaskTerminalReason::DispatchRejected);
					}
					State->NotifyStateChanged();
					return;
				}

				auto& ExecutionPool = State->GetTarget() == ETaskTarget::BlockingIO ? BlockingIOPool : Pool;
				const bool bAccepted = ExecutionPool.EnqueueNode(State, static_cast<EQueuedWorkPriority>(State->GetPriority()));
				if (bAccepted) State->NotifyStateChanged();
				if (!bAccepted)
				{
					requiref(IsTerminalState(State->GetState()), "Accepted task dispatch failed.");
					RecordRejectedTask(State->GetAttribution());
					State->FinishBody(ETaskState::Failed, "Task could not be queued because scheduler shutdown had begun.", ETaskTerminalReason::DispatchRejected);
				}
			}
			catch (const std::bad_alloc&)
			{
				requiref(false, "Task dispatch allocation failed.");
				std::terminate();
			}

		}

		auto ExecuteTask(
			const std::shared_ptr<FTaskStateData>& State,
			bool bWorkerExecution) -> void
		{
			if (!State->TryMarkRunning())
			{
				return;
			}
			State->ReleaseExecutionDependencies();
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
			DURIN_PROFILE_CPU_ZONE_NAMED("Task.Execute");

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

			// Hand off the slot at body return, but keep the limiter alive through
			// capture retirement, like UE's Pimpl capture in its low-level task.
			std::shared_ptr<Tasks::Private::FTaskConcurrencyLimiterState> LimiterLifetime;
			try
			{
				State->InvokeBody(State->MakeCancellationToken());
				LimiterLifetime = State->CompleteConcurrencyWork();
				State->ReleaseBody();
				ReleaseBodyStorage();
				State->FinishBody(ETaskState::Succeeded);
			}
			catch (const std::exception& Exception)
			{
				LimiterLifetime = State->CompleteConcurrencyWork();
				State->ReleaseBody();
				ReleaseBodyStorage();
				State->FinishBody(ETaskState::Failed, Exception.what(), ETaskTerminalReason::CallbackFailure);
			}
			catch (...)
			{
				LimiterLifetime = State->CompleteConcurrencyWork();
				State->ReleaseBody();
				ReleaseBodyStorage();
				State->FinishBody(ETaskState::Failed, "Task callable threw an unknown exception.", ETaskTerminalReason::CallbackFailure);
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

		auto RequestCancellation() -> void
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
				Task->RequestCancellation();
			}
		}

		auto Shutdown(bool bWaitForQueuedWork) -> void
		{
			CloseAdmission();
			if (!bWaitForQueuedWork) RequestCancellation();

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

			Pool.Destroy(true);
			BlockingIOPool.Destroy(true);
		}

		auto OnTaskTerminal(const std::shared_ptr<FTaskStateData>& State) -> void
		{
			const FTaskAccountingSnapshot Task = State->GetAccountingSnapshot();
			FTaskOwnerCategoryAggregate& Aggregate = GetAggregate(Task.Attribution);
			Aggregate.Subtract(ETaskAggregateGauge::Nonterminal, 1);
			if (Task.ExecutionState == ETaskState::Waiting) Aggregate.Subtract(ETaskAggregateGauge::Waiting, 1);
			else if (Task.ExecutionState == ETaskState::Queued) Aggregate.Subtract(ETaskAggregateGauge::Queued, 1);
			else if (Task.ExecutionState == ETaskState::Running) Aggregate.Subtract(ETaskAggregateGauge::Running, 1);
			Aggregate.Subtract(ETaskAggregateGauge::PayloadBytes, Task.PayloadBytes);
			if (Task.State == ETaskState::Succeeded) Aggregate.Increment(ETaskAggregateCounter::Succeeded);
			else if (Task.State == ETaskState::Failed) Aggregate.Increment(ETaskAggregateCounter::Failed);
			else Aggregate.Increment(ETaskAggregateCounter::Canceled);
			switch (Task.TerminalReason)
			{
			case ETaskTerminalReason::CancellationRequested: Aggregate.Increment(ETaskAggregateCounter::CancellationRequested); break;
			case ETaskTerminalReason::DispatchRejected: Aggregate.Increment(ETaskAggregateCounter::DispatchRejected); break;
			case ETaskTerminalReason::Superseded: Aggregate.Increment(ETaskAggregateCounter::Superseded); break;
			case ETaskTerminalReason::StaleGeneration: Aggregate.Increment(ETaskAggregateCounter::StaleGeneration); break;
			case ETaskTerminalReason::CallbackFailure: Aggregate.Increment(ETaskAggregateCounter::CallbackFailure); break;
			case ETaskTerminalReason::ShutdownCanceled: Aggregate.Increment(ETaskAggregateCounter::ShutdownCanceled); break;
			default: break;
			}
			if (Task.ExecutionState == ETaskState::Running)
			{
				Aggregate.Record(ETaskAggregateHistogram::Execution, Task.ExecutionNanoseconds);
			}

			std::lock_guard Lock(Mutex);
			if (State->GetTarget() == ETaskTarget::BlockingIO) { require(BlockingIOTrackedTasks > 0); --BlockingIOTrackedTasks; }
			auto& Storage = ExecutorStorage[static_cast<size_t>(State->GetTarget())];
			if (Task.ExecutionState != ETaskState::Running || State->IsExternal())
			{
				--Storage.CurrentPendingNodes;
				Storage.CurrentPendingBytes -= State->GetSchedulerStorageBytes();
			}
			--Storage.CurrentNodes;
			Storage.CurrentBytes -= State->GetSchedulerStorageBytes();
			const size_t RemovedTaskCount = ActiveTasks.erase(State->GetTaskId());
			check(RemovedTaskCount == 1);
			require(CurrentTrackedTaskCount > 0);
			--CurrentTrackedTaskCount;
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

		struct FRetractionWork
		{
			struct FFrame { std::shared_ptr<FTaskStateData> Task; bool bExecute = false; };
			std::vector<FFrame> Stack;
			std::vector<std::shared_ptr<FTaskStateData>> Dependencies;
			FTaskGraphVisited Visited;
		};

		auto RetractTaskBody(const std::shared_ptr<FTaskStateData>& Task) -> void
		{
			if (Task->PinScheduler().get() != this) return;
			if (Task->GetTarget() == ETaskTarget::AnyWorker) Pool.TryExecuteNode(Task);
			else if (Task->GetTarget() == ETaskTarget::BlockingIO && GExecutingBlockingIOPool) BlockingIOPool.TryExecuteNode(Task);
		}

		auto RetractTask(const std::shared_ptr<FTaskStateData>& Target, const FTaskWaitDeadline& Deadline,
			FRetractionWork& Work, const std::shared_ptr<FTaskStateData::FWaitSignal>& Signal = {}) -> void
		{
			Work.Stack.push_back({Target});
			while (!Work.Stack.empty() && !Deadline.IsExpired())
			{
				auto Frame = std::move(Work.Stack.back()); Work.Stack.pop_back();
				// Subscribe before inspecting a dependency: readiness/binding after
				// this point must invalidate the wait token even if this pass misses it.
				if (Signal) Frame.Task->RegisterWaiter(Signal);
				if (IsTerminalState(Frame.Task->GetState())) continue;
				if (!Frame.bExecute)
				{
					if (!Work.Visited.Visit(Frame.Task->GetTaskId())) continue;
					Work.Stack.push_back({Frame.Task, true});
				}
				else RetractTaskBody(Frame.Task);
				// The second visit also sees dynamic/nested work introduced by the body.
				Frame.Task->AppendRetractionDependencies(Work.Dependencies);
				for (auto& Dependency : Work.Dependencies) Work.Stack.push_back({std::move(Dependency)});
				Work.Dependencies.clear();
			}
			// Release pins between passes; retain only allocation capacity for this Wait.
			Work.Stack.clear();
			Work.Visited.Reset();
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
			{
				std::lock_guard Lock(Mutex);
				Snapshot.ExecutorStorage = ExecutorStorage;
				Snapshot.CurrentTrackedTaskCount = CurrentTrackedTaskCount;
				Snapshot.PeakTrackedTaskCount = PeakTrackedTaskCount;
				Snapshot.BlockingIOTrackedTasks = BlockingIOTrackedTasks;
			}
			Snapshot.TaskWarningThreshold = TaskWarningThreshold;
			Snapshot.QueueDepth = Pool.GetNumQueuedTasks();
			Snapshot.BlockingIOWorkerCount = BlockingIOPool.GetNumThreads();
			Snapshot.BlockingIOQueueDepth = BlockingIOPool.GetNumQueuedTasks();
			Snapshot.BlockingIOWarningThreshold = BlockingIOWarningThreshold;
			Snapshot.ActiveWorkerCount = ActiveWorkerCount.load(std::memory_order::acquire);
			Snapshot.CompletedTaskCount = CompletedTaskCount.load(std::memory_order::acquire);
			Snapshot.FailedTaskCount = FailedTaskCount.load(std::memory_order::acquire);
			Snapshot.CanceledTaskCount = CanceledTaskCount.load(std::memory_order::acquire);
			Snapshot.RejectedTaskCount = RejectedTaskCount.load(std::memory_order::acquire);
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
		uint32 BlockingIOWarningThreshold = 0;
		uint32 BlockingIOTrackedTasks = 0;
		mutable std::mutex Mutex;
		std::condition_variable QuiescenceCV;
		uint64 PendingSubmissions = 0;
		std::unordered_map<uint64, std::shared_ptr<FTaskStateData>> ActiveTasks;
		std::shared_ptr<FTaskSchedulerLifetimeAccounting> LifetimeAccounting = std::make_shared<FTaskSchedulerLifetimeAccounting>();
		std::array<FTaskOwnerCategoryAggregate, MaxTaskAttributionPairs> OwnerCategoryAggregates{};
		std::atomic<uint32> ActiveWorkerCount = 0;
		std::atomic<uint64> CompletedTaskCount = 0;
		std::atomic<uint64> FailedTaskCount = 0;
		std::atomic<uint64> CanceledTaskCount = 0;
		std::atomic<uint64> RejectedTaskCount = 0;
		uint64 CurrentTrackedTaskCount = 0;
		uint64 PeakTrackedTaskCount = 0;
		std::atomic<uint64> LongWaitCount = 0;
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		uint32 WorkerCount = 0;
		std::array<FTaskExecutorStorageDiagnostics, 3> ExecutorStorage;
		uint64 TaskWarningThreshold = 0;
		bool bAcceptingTasks = false;
	};

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

		auto ValidateLifetime() -> std::optional<Tasks::FTaskConstructionError>
		{
			std::lock_guard Lock(Mutex);
			if (!bAccepting) return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::LifetimeClosed};
			return {};
		}

		auto Enqueue(const std::shared_ptr<FTaskStateData>& State) -> bool
		{
			auto Entry = std::make_shared<FEntry>();
			Entry->State = State;
			Entry->PayloadBytes = sizeof(FEntry) + State->GetSchedulerStorageBytes();
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
				if (!bAccepting)
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
				const bool bWasOverloaded = ActiveEntryCount > Config.QueuedEntriesWarningThreshold
					|| QueuedPayloadBytes > Config.QueuedBytesWarningThreshold;
				const bool bWillOverload = ActiveEntryCount - ReplacedEntries + 1 > Config.QueuedEntriesWarningThreshold
					|| QueuedPayloadBytes - ReplacedBytes + PayloadBytes > Config.QueuedBytesWarningThreshold;
				if ((!bWasOverloaded && bWillOverload) || PayloadBytes > Config.EntryBytesWarningThreshold)
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
				SupersededState->FinishBody(ETaskState::Failed, "GameThread deferred task was superseded.", ETaskTerminalReason::Superseded);
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
					Entry->State->FinishBody(ETaskState::Failed, "GameThread deferred task generation expired.", ETaskTerminalReason::StaleGeneration);
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
					Entry->State->FinishBody(ETaskState::Failed, "Task scheduler was unavailable during GameThread dispatch.", ETaskTerminalReason::DispatchRejected);
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

	static auto ValidateTaskExecution(ETaskTarget Target, ETaskPriority Priority)
		-> std::optional<Tasks::FTaskConstructionError>
	{
		using ECode = Tasks::ETaskConstructionErrorCode;
		if ((Target != ETaskTarget::AnyWorker && Target != ETaskTarget::GameThreadDeferred && Target != ETaskTarget::BlockingIO)
			|| (Priority != ETaskPriority::High && Priority != ETaskPriority::Normal && Priority != ETaskPriority::Low))
			return Tasks::FTaskConstructionError{ECode::UnsupportedExecutor};
		if (Target == ETaskTarget::AnyWorker || Target == ETaskTarget::BlockingIO) return {};
		std::shared_ptr<FGameThreadDeferredWorkQueue> Queue;
		{
			std::lock_guard Lock(GGameThreadDeferredQueueMutex);
			Queue = GGameThreadDeferredQueue;
		}
		if (!Queue) return Tasks::FTaskConstructionError{ECode::UnsupportedExecutor};
		return Queue->ValidateLifetime();
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

	namespace
	{
		// Match UE's bounded synchronous Close propagation. Deferred nodes stay pinned
		// until the outermost close drains them on the same thread.
		constexpr uint32 MaxTaskCloseDepth = 128;
		struct FTaskCloseWork
		{
			uint32 Depth = 0;
			std::vector<std::shared_ptr<FTaskStateData>> Pending;
		};
		thread_local FTaskCloseWork GTaskCloseWork;
	}

	auto FTaskStateData::FinishBody(ETaskState TerminalState, std::string InDiagnostic, ETaskTerminalReason InReason) -> bool
	{
		{
			std::lock_guard Lock(Mutex);
			if (Phase != EPhase::Open) return false;
			check(ExecutionState == ETaskState::Running || TerminalState == ETaskState::Failed);
			if (TerminalState != ETaskState::Succeeded || Outcome.State != ETaskState::Canceled)
				Outcome = {TerminalState, std::move(InDiagnostic), InReason};
			Phase = EPhase::BodyFinished;
			if (Event && PendingNestedCount != 0) Event->CompletionLifetime = shared_from_this();
		}
		TryClose();
		return true;
	}

	auto FTaskStateData::TryClose() -> void
	try
	{
		{
			std::lock_guard Lock(Mutex);
			if (Phase != EPhase::BodyFinished || PendingNestedCount != 0) return;
			Phase = EPhase::Closing;
			FinishTimeNanoseconds = MonotonicNanoseconds();
		}
		auto& Work = GTaskCloseWork;
		if (Work.Depth >= MaxTaskCloseDepth)
		{
			Work.Pending.push_back(shared_from_this());
			return;
		}
		++Work.Depth;
		Close();
		if (--Work.Depth != 0) return;
		// Keep a depth sentinel while draining so nested closes do not start drains.
		++Work.Depth;
		std::vector<std::shared_ptr<FTaskStateData>> Batch;
		while (!Work.Pending.empty())
		{
			Batch.swap(Work.Pending);
			for (const auto& Task : Batch) Task->Close();
			Batch.clear();
		}
		std::vector<std::shared_ptr<FTaskStateData>>{}.swap(Work.Pending);
		--Work.Depth;
	}
	catch (const std::bad_alloc&)
	{
		requiref(false, "Task close allocation failed.");
		std::terminate();
	}

	auto FTaskStateData::Close() noexcept -> void
	{
		Private::FTaskCompletionFunction Function;
		{
			std::lock_guard Lock(Mutex);
			check(Phase == EPhase::Closing && PendingNestedCount == 0);
			Function = std::move(CompletionFunction);
		}
		InvokeTaskTerminalPublicationTestHook(TaskId);
		if (Function) Function(Outcome.State);
		Function = {};
		ReleaseBody();

		std::vector<FTaskSubsequent> SubsequentNodes;
		std::vector<std::shared_ptr<FTaskStateData>> ReleasedDependencies;
		std::shared_ptr<FTaskStateData> EventLifetime;
		for (;;)
		{
			std::shared_ptr<Private::FTaskTerminalHook> Hooks;
			{
				std::lock_guard Lock(Mutex);
				Hooks = std::move(TerminalHooks);
				if (!Hooks)
				{
					// Closing the hook/dependent lists and publishing completion share
					// one linearization point. Registration never waits for user hooks.
					LifetimeAccounting->RetainedTerminalTaskCount.fetch_add(1, std::memory_order_acq_rel);
					if (bHasResultStorage)
					{
						LifetimeAccounting->RetainedTerminalResultCount.fetch_add(1, std::memory_order_acq_rel);
					}
					ReleasedDependencies = Dependencies.Release(FTaskDependencyEdges::ERelease::Closed);
					Phase = EPhase::Closed;
					if (bPipeCounted) { Pipe->RemoveTask(); bPipeCounted = false; }
					SubsequentNodes = std::move(Subsequents);
					if (Event) EventLifetime = std::move(Event->CompletionLifetime);
					break;
				}
			}
			while (Hooks)
			{
				auto Hook = std::move(Hooks);
				Hooks = std::move(Hook->Next);
				Hook->Function(Outcome.State);
			}
		}
		if (auto Limiter = std::move(ConcurrencyLimiter)) Limiter->RemoveTask(ConcurrencySlot);
		// Completion is visible before nested/result destruction, as in UE Close.
		ReleasedDependencies.clear();
		NotifyStateChanged();
		for (const auto& Subsequent : SubsequentNodes)
			if (auto Pinned = Subsequent.Node.lock())
			{
				if (Subsequent.Kind == ETaskDependencyKind::Execution) Pinned->OnPrerequisiteTerminal();
				else if (Subsequent.Kind == ETaskDependencyKind::Pipe) Pinned->OnPipePredecessorTerminal();
				else Pinned->OnNestedCompleted();
			}
		if (auto PinnedScheduler = Scheduler.lock()) PinnedScheduler->OnTaskTerminal(shared_from_this());
	}

	auto FTaskStateData::OnPipePredecessorTerminal() -> void
	{
		{
			std::lock_guard Lock(Mutex);
			check(bPipeWaiting && bQueueClaimed);
			bPipeWaiting = false;
			bQueueClaimed = false;
		}
		if (auto PinnedScheduler = Scheduler.lock()) PinnedScheduler->QueueTask(shared_from_this());
	}

	auto FTaskStateData::OnConcurrencyPermit(uint32 Slot) -> void
	{
		{
			std::lock_guard Lock(Mutex);
			ConcurrencySlot = Slot;
			bQueueClaimed = false;
		}
		if (auto Owner = Scheduler.lock()) Owner->QueueTask(shared_from_this());
	}

	auto Tasks::Private::FTaskConcurrencyLimiterState::RemoveTask(uint32 Slot) -> void
	{
		std::shared_ptr<FTaskStateData> Next;
		{
			std::lock_guard Lock(Mutex);
			require(TaskCount > 0);
			--TaskCount;
			if (Slot != std::numeric_limits<uint32>::max())
			{
				Scheduled[Slot].reset();
				while (!Pending.empty() && !Next)
				{
					Next = Pending.front().lock();
					Pending.pop_front();
				}
				if (Next) Scheduled[Slot] = Next;
				else FreeSlots.push_back(Slot);
			}
			if (auto Signal = WaitSignal.lock()) Signal->Notify();
		}
		if (Next) Next->OnConcurrencyPermit(Slot);
	}

	Tasks::FTaskConcurrencyLimiter::FTaskConcurrencyLimiter(uint32 MaxConcurrency, ETaskPriority Priority)
	try : State(std::make_shared<Private::FTaskConcurrencyLimiterState>())
	{
		require(MaxConcurrency > 0);
		require(Priority == ETaskPriority::High || Priority == ETaskPriority::Normal || Priority == ETaskPriority::Low);
		State->Scheduled.resize(MaxConcurrency);
		State->FreeSlots.reserve(MaxConcurrency);
		for (uint32 Slot = MaxConcurrency; Slot != 0; --Slot) State->FreeSlots.push_back(Slot - 1);
		State->Priority = Priority;
	}
	catch (const std::bad_alloc&) { Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::AllocationFailure}); }
	Tasks::FTaskConcurrencyLimiter::~FTaskConcurrencyLimiter() = default;

	auto Tasks::FTaskConcurrencyLimiter::GetCurrentSlot() -> uint32
	{ return GCurrentTaskState->GetConcurrencySlot(); }

	auto Tasks::FTaskConcurrencyLimiter::PushImpl(const char* Name, Durin::Private::FMoveOnlyTaskFunction&& Function,
		FTaskCancellationToken Cancellation) -> void
	{
		FTaskLaunchOptions Options;
		Options.ConcurrencyLimiter = State;
		Options.Priority = State->Priority;
		Options.CancellationToken = std::move(Cancellation);
		(void)Durin::Private::LaunchCancelableTaskWithCompletion(Name, std::move(Function), {}, Options);
	}

	auto Tasks::FPipe::AddTask() -> void
	{ std::lock_guard Lock(Mutex); ++TaskCount; }
	auto Tasks::FPipe::RemoveTask() -> void
	{
		std::lock_guard Lock(Mutex);
		check(TaskCount > 0);
		if (--TaskCount == 0) Empty.notify_all();
	}
	Tasks::FPipe::~FPipe()
	{ requiref(!HasWork(), "A pipe must outlive all of its tasks."); }
	auto Tasks::FPipe::HasWork() const -> bool
	{ std::lock_guard Lock(Mutex); return TaskCount != 0; }
	auto Tasks::FPipe::IsInContext() const -> bool
	{ return GCurrentTaskState && GCurrentTaskState->GetPipe() == this; }
	auto Tasks::FPipe::WaitUntilEmpty(double TimeoutSeconds) -> bool
	{
		std::unique_lock Lock(Mutex);
		if (TaskCount == 0) return true;
		if (IsInContext() || (GCurrentTaskState && GCurrentTaskState->IsIndependentCPU())
			|| IsInRenderingThread() || !(TimeoutSeconds > 0)) return false;
		if (std::isinf(TimeoutSeconds)) { Empty.wait(Lock, [&] { return TaskCount == 0; }); return true; }
		return Empty.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), [&] { return TaskCount == 0; });
	}

	auto FTaskStateData::OnPrerequisiteTerminal() -> void
	{
		bool bShouldQueue = false;
		{
			std::lock_guard Lock(Mutex);
			if (Phase != EPhase::Open || ExecutionState != ETaskState::Waiting)
			{
				return;
			}
			check(RemainingPrerequisites > 0);
			--RemainingPrerequisites;
			if (RemainingPrerequisites == 0)
			{
				ExecutionState = ETaskState::Queued;
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
				FinishBody(ETaskState::Failed, "Task scheduler was unavailable when prerequisites completed.", ETaskTerminalReason::DispatchRejected);
			}
		}
	}

	auto FTaskStateData::RequestCancellation() -> bool
	{
		{
			std::lock_guard Lock(Mutex);
			if (Phase >= EPhase::Closing) return false;
			bCancellationRequested = true;
		}
		return true;
	}

	auto FTaskStateData::OnNestedCompleted() -> void
	{
		{
			std::lock_guard Lock(Mutex);
			require(PendingNestedCount != 0);
			if (--PendingNestedCount != 0) return;
		}
		TryClose();
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

	auto Private::RetainForwardedTask(const Tasks::FTask& Owner, Tasks::FTask Inner) -> void
	{
		struct FForwardedLifetime
		{
			Tasks::FTask Task;
			~FForwardedLifetime() { ReleaseForwardedTask(std::move(Task)); }
		};
		auto Lifetime = std::make_shared<FForwardedLifetime>();
		Lifetime->Task = std::move(Inner);
		FTaskRuntimeAccess::RetainLifetime(Owner, std::move(Lifetime));
	}

	auto Private::ReleaseForwardedTask(Tasks::FTask Task) noexcept -> void
	{
		if (!Task.IsValid()) return;
		thread_local bool bDraining = false;
		thread_local std::vector<Tasks::FTask> Pending;
		if (bDraining)
		{
			Pending.push_back(std::move(Task));
			return;
		}
		bDraining = true;
		Task = {};
		while (!Pending.empty())
		{
			auto Next = std::move(Pending.back());
			Pending.pop_back();
			Next = {};
		}
		bDraining = false;
	}

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
	auto Tasks::Private::FTaskHandle::IsCancellationRequested() const -> bool
	{
		return State && State->IsCancellationRequested();
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
		if (Config.TaskWarningThreshold == 0 || Config.NumBlockingIOThreads == 0 || Config.BlockingIOWarningThreshold == 0)
		{
			DURIN_ERROR("Task scheduler initialization rejected because thread count and warning thresholds must be positive.");
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
			"Task scheduler initialized: workers={} | warning threshold={}",
			Config.NumWorkerThreads > 0 ? Config.NumWorkerThreads : GetDefaultThreadPoolThreadCount(),
			Config.TaskWarningThreshold);
		return true;
	}

	auto InitializeGameThreadDeferredExecutor(const FGameThreadDeferredWorkQueueConfig& Config) -> bool
	{
		CheckGameThread();
		if (Config.QueuedEntriesWarningThreshold == 0 || Config.QueuedBytesWarningThreshold == 0
			|| Config.EntryBytesWarningThreshold == 0 || Config.FrameMaxCallbacks == 0
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

		if (Mode == ETaskShutdownMode::Cancel) SchedulerToDestroy->RequestCancellation();
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
		if (!Profiling::GetConnectionState().bConnected) return;
		std::shared_ptr<FTaskScheduler> Scheduler;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			// Bound snapshot collection itself, not just publication of the plots.
			static auto NextSampleTime = std::chrono::steady_clock::time_point{};
			const auto Now = std::chrono::steady_clock::now();
			if (Now < NextSampleTime) return;
			NextSampleTime = Now + std::chrono::milliseconds(100);
			Scheduler = GTaskScheduler;
		}
		if (Scheduler) Scheduler->PublishProfilerPlots();
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

		// Covers preparation, publication, and rejected callable destruction during shutdown.
		struct FSubmissionGuard
		{
			std::shared_ptr<FTaskScheduler> Scheduler;
			~FSubmissionGuard() { if (Scheduler) Scheduler->EndSubmission(); }
		};

		auto LaunchCancelableTaskWithCompletion(
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			uint64 AdditionalStorageBytes, FTaskResultStorage Result, const Tasks::FTask* PrimaryPrerequisite) -> Tasks::FTask
		try
		{
			{
				if (auto Error = ValidateTaskExecution(Options.Target, Options.Priority))
					Tasks::Detail::FailConstruction(*Error);
			}
			FTaskLaunchOptions ResolvedOptions = Options;
			if (FTaskAttributionAccess::IsDefault(ResolvedOptions.Attribution) && GCurrentTaskState && GCurrentTaskScheduler)
			{
				ResolvedOptions.Attribution = GCurrentTaskState->GetAttribution();
			}
			if (!Function)
			{
				DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
				Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::InvalidCallable});
			}
			FSubmissionGuard Submission;

			{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				DURIN_WARN("Task launch failed because the task scheduler is not running. (task: {})", Name ? Name : "");
				Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::LifetimeClosed});
			}

			Submission.Scheduler = GTaskScheduler;
			Submission.Scheduler->BeginSubmission();
			}
			std::shared_ptr<FTaskStateData> State = Submission.Scheduler->Submit(
				Name,
				Function,
				std::move(CompletionFunction),
				ResolvedOptions,
				Options.Target,
				Options.Priority,
				Options.EstimatedPayloadBytes,
				AdditionalStorageBytes, Options.GenerationToken, Options.CoalescingKey, std::move(Result), PrimaryPrerequisite
			);
			return Tasks::FTask(std::move(State));
		}
		catch (const std::bad_alloc&)
		{
			Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::AllocationFailure});
		}


		auto LaunchContinuationTask(
			const Tasks::FTask& Predecessor,
			const char* Name,
			FMoveOnlyTaskFunction&& Function,
			Private::FTaskCompletionFunction&& CompletionFunction,
			const FTaskContinuationOptions& Options,
			uint64 AdditionalStorageBytes, FTaskResultStorage Result) -> Tasks::FTask
		try
		{
			FTaskAttribution ResolvedAttribution = FTaskAttributionAccess::IsDefault(Options.Attribution) && Predecessor.State
				? Predecessor.State->GetAttribution()
				: Options.Attribution;

			FTaskLaunchOptions LaunchOptions;
			LaunchOptions.Prerequisites = Options.Prerequisites;
			LaunchOptions.CancellationToken = Options.CancellationToken;
			LaunchOptions.Attribution = ResolvedAttribution;

			LaunchOptions.Target = Options.Target;
			LaunchOptions.Priority = Options.Priority;
			LaunchOptions.EstimatedPayloadBytes = Options.EstimatedPayloadBytes;
			LaunchOptions.GenerationToken = Options.GenerationToken;
			LaunchOptions.CoalescingKey = Options.CoalescingKey;
			return LaunchCancelableTaskWithCompletion(Name, std::move(Function), std::move(CompletionFunction),
				LaunchOptions, AdditionalStorageBytes, std::move(Result), &Predecessor);
		}
		catch (const std::bad_alloc&)
		{
			Tasks::Detail::FailConstruction({Tasks::ETaskConstructionErrorCode::AllocationFailure});
		}

		auto FTaskRuntimeAccess::GetFailure(const Tasks::FTask& Task) -> Tasks::FTaskFailure
		{ require(Task.State); return Task.State->GetFailure(); }
		auto FTaskRuntimeAccess::SetFailure(const Tasks::FTask& Task, Tasks::FTaskFailure Failure) -> void
		{ require(Task.State); Task.State->SetFailure(std::move(Failure)); }
		auto FTaskRuntimeAccess::SetCurrentFailure(Tasks::FTaskFailure Failure) -> void
		{ require(GCurrentTaskState); GCurrentTaskState->SetFailure(std::move(Failure)); }

		auto FTaskRuntimeAccess::PinLifetime(const Tasks::FTask& Task) -> std::shared_ptr<void>
		{ require(Task.State); return Task.State; }
		auto FTaskRuntimeAccess::RetainLifetime(const Tasks::FTask& Task, std::shared_ptr<void> Lifetime) -> void
		{ require(Task.State); Task.State->RetainLifetime(std::move(Lifetime)); }

		auto FTaskRuntimeAccess::GetAttribution(const Tasks::FTask& Task) -> FTaskAttribution { return Task.State ? Task.State->GetAttribution() : FTaskAttribution{}; }

		auto FTaskRuntimeAccess::IsExecutingIndependentCPU() -> bool { return GCurrentTaskState && GCurrentTaskState->IsIndependentCPU(); }
		auto FTaskRuntimeAccess::GetCurrentTaskId() -> uint64 { return GCurrentTaskState && GCurrentTaskState->IsRunningBody() ? GCurrentTaskState->GetTaskId() : 0; }



		auto FTaskRuntimeAccess::CreateTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement) -> Tasks::FTask
		{
			FTaskNodeInit Init;
			Init.DebugName = DebugName;
			Init.LifetimeAccounting = std::make_shared<FTaskSchedulerLifetimeAccounting>();
			Init.Kind = ETaskNodeKind::Event;
			Init.bUnknownExecutionRequirement = bUnknownExecutionRequirement;
			return Tasks::FTask(std::make_shared<FTaskStateData>(Init));
		}

		auto FTaskRuntimeAccess::AddTaskEventPrerequisites(const Tasks::FTask& Task, std::span<const Tasks::FTask> Prerequisites)
			-> std::optional<Tasks::FTaskConstructionError>
		{
			require(Task.State && Task.State->Event);
			std::lock_guard Lock(Task.State->Event->SignalMutex);
			requiref(!Task.State->Event->Triggered, "Task event prerequisites must be added before Trigger.");
			for (const auto& Prerequisite : Prerequisites)
				if (auto Error = AddCompletionDependency(Task, Prerequisite)) return Error;
			return {};
		}

		auto FTaskRuntimeAccess::TriggerTaskEvent(const Tasks::FTask& Task) -> void
		{
			auto State = Task.State;
			if (!State) return; // Empty/moved handles are already completed, as in UE.
			require(State->Event);
			{
				std::lock_guard Lock(State->Event->SignalMutex);
				if (State->Event->Triggered) return;
				State->Event->Triggered = true;
			}
			// Publication may call user hooks; never hold the event's signal lock here.
			State->FinishBody(ETaskState::Succeeded);
		}

		auto FTaskRuntimeAccess::BindTerminal(const Tasks::FTask& Task, std::shared_ptr<FTaskTerminalHook> Hook) -> void
		{
			require(Task.State && Hook && Hook->Function);
			Task.State->BindTerminal(std::move(Hook));
		}
		auto FTaskRuntimeAccess::CompleteExternal(const Tasks::FTask& Task, ETaskState Terminal) -> void
		{
			require(Task.State && Task.State->IsExternal());
			if (Terminal == ETaskState::Failed) Task.State->FinishBody(ETaskState::Failed, "External task source failed or was abandoned.", ETaskTerminalReason::CallbackFailure);
			else
			{
				Task.State->FinishBody(Terminal);
			}
		}

		auto FTaskRuntimeAccess::BindDynamicDependency(const Tasks::FTask& Task, const Tasks::FTask& Inner)
			-> std::optional<Tasks::FTaskConstructionError>
		{
			// Serializes dynamic edge insertion and cycle checks across concurrent sources.
			{
				std::lock_guard Lock(GTaskDependencyMutex);
				if (!Task.State || !Inner.State || Task.State->PinScheduler() != Inner.State->PinScheduler())
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidPrerequisite, Inner.GetTaskId()};
				if (Task.State->IsIndependentCPU())
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
				if (Task.State == Inner.State || Inner.State->DependsOn(Task.State.get()))
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::DependencyCycle, Inner.GetTaskId()};
				if (!Task.State->SetDynamicDependency(Inner.State))
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
			}
			// Wake graph waiters only after publishing the dependency.
			Task.State->NotifyStateChanged();
			return {};
		}

		auto FTaskRuntimeAccess::AddCompletionDependency(const Tasks::FTask& Parent, const Tasks::FTask& Child)
			-> std::optional<Tasks::FTaskConstructionError>
		{
			std::lock_guard GraphLock(GTaskDependencyMutex);
			if (Parent.State && Parent.State->IsTaskEvent() && !Child.State) return {};
			if (!Parent.State || !Child.State
				|| (!Parent.State->IsTaskEvent() && !Child.State->IsTaskEvent() && Parent.State->PinScheduler() != Child.State->PinScheduler()))
				return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidPrerequisite, Child.GetTaskId()};
			if (Parent.State->IsIndependentCPU())
				return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
			// A completed child imposes no dependency, including an already-canceled back-edge.
			if (IsTerminalState(Child.State->GetState()))
			{
				std::lock_guard Lock(Parent.State->Mutex);
				if (Parent.State->ExecutionState != ETaskState::Running || Parent.State->Phase != FTaskStateData::EPhase::Open)
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
				return {};
			}
			if (Parent.State == Child.State || Child.State->DependsOn(Parent.State.get()))
				return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::DependencyCycle, Child.GetTaskId()};
			{
				std::scoped_lock Lock(Parent.State->Mutex, Child.State->Mutex);
				if (Parent.State->ExecutionState != ETaskState::Running || Parent.State->Phase != FTaskStateData::EPhase::Open)
					return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
				if (Child.State->Phase == FTaskStateData::EPhase::Closed) return {};
				// Preserve ordinary admission reservations while adding an unreserved nested edge.
				Parent.State->Dependencies.ReserveAdditional();
				Child.State->ReserveSubsequentLocked();
				Parent.State->Dependencies.Add(Child.State, Child.GetTaskId(), ETaskDependencyKind::Nested);
				Child.State->RegisterDependentLocked(Parent.State, ETaskDependencyKind::Nested);
				++Parent.State->PendingNestedCount;
			}
			Parent.State->NotifyStateChanged();
			return {};
		}

		auto FTaskRuntimeAccess::AddNested(const Tasks::FTask& Child) -> std::optional<Tasks::FTaskConstructionError>
		{
			if (!GCurrentTaskState || !GCurrentTaskState->IsRunningBody())
				return Tasks::FTaskConstructionError{Tasks::ETaskConstructionErrorCode::InvalidExecutionContract};
			return AddCompletionDependency(Tasks::FTask(GCurrentTaskState->shared_from_this()), Child);
		}

		auto FTaskRuntimeAccess::IsCancellationRequested(const Tasks::FTask& Task) -> bool
		{
			return Task.State && Task.State->MakeCancellationToken().IsCancellationRequested();
		}

		auto FTaskRuntimeAccess::SetCurrentCanceledOutcome() -> void
		{
			require(GCurrentTaskState);
			GCurrentTaskState->SetCanceledOutcome();
		}


	} // namespace Private

	auto CancelTask(const Tasks::FTask& Task) -> bool
	{
		return Task.State && Task.State->RequestCancellation();
	}

	auto WaitTask(const Tasks::FTask& Task) -> FTaskWaitResult
	{ return WaitTask(Task, std::numeric_limits<double>::infinity()); }

	namespace
	{
		auto GetTaskWaitRestriction(const std::shared_ptr<FTaskStateData>& TaskState,
			FTaskStateData::FWaitGraphWork& Work) -> std::optional<ETaskWaitStatus>
		{
			if (GCurrentTaskState == TaskState.get())
			{
				DURIN_WARN("Task wait rejected because a task cannot wait for itself. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::SelfWait;
			}

			const bool bGameThread = GIsGameThreadIdInitialized && IsInGameThread();
			const auto Requirements = GCurrentTaskState || bGameThread
				? TaskState->InspectWaitGraph(GCurrentTaskState, Work) : FTaskStateData::FWaitRequirements{};
			if (Requirements.bCycle)
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
			if (GCurrentTaskState && Requirements.bUnknown)
				return ETaskWaitStatus::UnsupportedThread;
			if (bGameThread && (Requirements.bUnknown || Requirements.bGameThread))
			{
				DURIN_WARN("Task wait rejected because GameThread cannot block on deferred GameThread work. (task: {}, id: {})", TaskState->GetDebugName(), TaskState->GetTaskId());
				return ETaskWaitStatus::UnsupportedThread;
			}

			return std::nullopt;
		}

		auto GetRetractionScheduler(const std::shared_ptr<FTaskStateData>& TaskState) -> std::shared_ptr<FTaskScheduler>
		{
			if (auto Scheduler = TaskState->PinScheduler()) return Scheduler;
			if (!TaskState->IsTaskEvent()) return {};
			if (GCurrentTaskScheduler) return GCurrentTaskScheduler->shared_from_this();
			std::lock_guard Lock(GTaskSchedulerMutex);
			return GTaskScheduler;
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

			FTaskStateData::FWaitGraphWork WaitGraph;
			if (auto Restriction = GetTaskWaitRestriction(TaskState, WaitGraph)) return {*Restriction, State};

			std::shared_ptr<FTaskScheduler> Scheduler = GetRetractionScheduler(TaskState);
			auto Signal = std::make_shared<FTaskStateData::FWaitSignal>();
			TaskState->RegisterWaiter(Signal);
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
						"Long task wait: waiter={} | target={} | task={} | state={} | {} ms",
						WaiterName,
						TargetDiagnostics.DebugName,
						TargetDiagnostics.TaskId,
						static_cast<uint32>(TargetDiagnostics.State),
						ElapsedNanoseconds / 1'000'000
					);
				}
			};
			FTaskScheduler::FRetractionWork Retraction;
			while (!IsTerminalState(State))
			{
				// Prepare before inspecting/retracting so a concurrent enqueue, binding
				// or publication between inspection and sleep cannot be lost.
				const uint64 Observed = Signal->PrepareWait();
				if (Scheduler && !Deadline.IsExpired())
					Scheduler->RetractTask(TaskState, Deadline, Retraction, Signal);
				State = TaskState->GetState();
				ReportLongWait();
				if (IsTerminalState(State)) break;
				if (Deadline.IsExpired()) return {ETaskWaitStatus::TimedOut, State};
				// Retraction and concurrent bodies may introduce deferred or external dependencies.
				if (auto Restriction = GetTaskWaitRestriction(TaskState, WaitGraph)) return {*Restriction, State};
				const double UntilDiagnostic = bReportedLongWait ? std::numeric_limits<double>::infinity()
					: std::max(0.0, LongWaitThresholdSeconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - WaitStartTime).count());
				const double Timeout = std::min(Deadline.Remaining(), UntilDiagnostic);
				Signal->WaitFor(Observed, Timeout);
			}
			return {ETaskWaitStatus::Completed, State};
		}
	}

	auto Tasks::FTaskConcurrencyLimiter::Wait(double TimeoutSeconds) -> bool
	{
		const FTaskWaitDeadline Deadline(TimeoutSeconds);
		std::shared_ptr<FTaskWaitSignal> Signal;
		{
			std::lock_guard Lock(State->Mutex);
			if (State->TaskCount == 0) return true;
			if ((GCurrentTaskState && (GCurrentTaskState->GetConcurrencyLimiter() == State.get() || GCurrentTaskState->IsIndependentCPU()))
				|| IsInRenderingThread() || Deadline.IsExpired()) return false;
			Signal = State->WaitSignal.lock();
			if (!Signal) State->WaitSignal = Signal = std::make_shared<FTaskWaitSignal>();
		}
		for (;;)
		{
			const uint64 Observed = Signal->PrepareWait();
			// Drain all scheduled bodies before blocking, as UE 5.8 does before
			// checking its one-shot event. Nested completion is outside the limiter.
			for (size_t Slot = 0; Slot < State->Scheduled.size(); ++Slot)
			{
				std::shared_ptr<FTaskStateData> Task;
				{
					std::lock_guard Lock(State->Mutex);
					if (State->TaskCount == 0) return true;
					if (Deadline.IsExpired()) return false;
					Task = State->Scheduled[Slot].lock();
				}
				if (!Task) continue;
				// Slot publication precedes enqueue. Subscribe before trying execution
				// so a concurrent enqueue cannot leave this waiter asleep.
				Task->RegisterWaiter(Signal);
				if (auto Scheduler = Task->PinScheduler()) Scheduler->RetractTaskBody(Task);
			}
			{
				std::lock_guard Lock(State->Mutex);
				if (State->TaskCount == 0) return true;
				if (Deadline.IsExpired()) return false;
			}
			Signal->WaitFor(Observed, Deadline.Remaining());
		}
	}

	auto Tasks::Private::FTaskHandle::TryRetractAndExecute() const -> bool
	{
		if (!State || IsTerminalState(State->GetState())) return true;
		FTaskStateData::FWaitGraphWork WaitGraph;
		if (GetTaskWaitRestriction(State, WaitGraph)) return false;
		if (auto Scheduler = GetRetractionScheduler(State))
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
		// TimedOut represents an unfinished input until the shared deadline expires.
		std::vector<FTaskWaitResult> Outcomes(Tasks.size(), {ETaskWaitStatus::TimedOut, ETaskState::Invalid});
		FTaskStateData::FWaitGraphWork WaitGraph;
		auto Observe = [&](size_t Index) {
			auto& Outcome = Outcomes[Index];
			if (Outcome.WaitStatus != ETaskWaitStatus::TimedOut) return false;
			const auto& State = Tasks[Index].State;
			Outcome.TaskState = State ? State->GetState() : ETaskState::Invalid;
			if (!State || IsTerminalState(Outcome.TaskState)) Outcome.WaitStatus = ETaskWaitStatus::Completed;
			else if (Deadline.IsExpired()) return false;
			else if (auto Restriction = GetTaskWaitRestriction(State, WaitGraph)) Outcome.WaitStatus = *Restriction;
			else return true;
			return false;
		};
		bool bPending = false;
		for (size_t Index = 0; Index < Tasks.size(); ++Index) bPending |= Observe(Index);
		if (!bPending) return Outcomes;

		auto Signal = std::make_shared<FTaskWaitSignal>();
		FTaskScheduler::FRetractionWork Retraction;
		for (;;)
		{
			const uint64 Observed = Signal->PrepareWait();
			bPending = false;
			for (size_t Index = 0; Index < Tasks.size(); ++Index)
			{
				if (!Observe(Index)) continue;
				const auto& State = Tasks[Index].State;
				State->RegisterWaiter(Signal);
				if (auto Scheduler = GetRetractionScheduler(State)) Scheduler->RetractTask(State, Deadline, Retraction, Signal);
				bPending |= Observe(Index);
			}
			if (!bPending) return Outcomes;
			Signal->WaitFor(Observed, Deadline.Remaining());
		}
	}

	namespace Private
	{
		struct FParallelForAccess
		{
			template<typename F>
			static auto Execute(const char* Name, uint64 Num, F&& Function, const FParallelForOptions& Options) -> FParallelForResult;
		};
	}

	template<typename F>
	auto Private::FParallelForAccess::Execute(const char* Name, uint64 Num, F&& Function, const FParallelForOptions& Options) -> FParallelForResult
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

		const bool bNested = GParallelForDepth > 0;
		FParallelForDepthScope CallerDepthScope;
		const uint64 MinBatchSize = std::max<uint64>(1, Options.MinBatchSize);
		const uint64 BatchLimitedChunks = 1 + (Num - 1) / MinBatchSize;
		const uint64 WorkerLimitedChunks = bSchedulerRunning ? static_cast<uint64>(SchedulerWorkerCount) + 1 : 1;
		const uint32 ChunkCount = static_cast<uint32>(std::min<uint64>(Num, std::min(BatchLimitedChunks, WorkerLimitedChunks)));
		const uint32 EffectiveChunkCount = bNested ? 1 : std::max<uint32>(1, ChunkCount);
		const FTaskCancellationToken ExternalToken = Options.CancellationToken;
		auto Invoke = [](F& Body, uint64 Index, const FParallelForCancellationToken& Token) {
			if constexpr (std::is_invocable_v<F&, uint64, const FParallelForCancellationToken&>) Body(Index, Token);
			else Body(Index);
		};
		auto FailureDiagnostic = [](uint64 Start, uint64 End, const char* Diagnostic) {
			return "ParallelFor chunk [" + std::to_string(Start) + ", " + std::to_string(End) + ") failed: " + Diagnostic;
		};
		// Like UE 5.8, single-thread execution precedes all parallel shared storage.
		if (EffectiveChunkCount == 1)
		{
			auto Body = std::move(Function);
			const FParallelForCancellationToken Token({}, ExternalToken);
			try
			{
				for (uint64 Index = 0; Index < Num && !Token.IsCancellationRequested(); ++Index) Invoke(Body, Index, Token);
			}
			catch (const std::exception& Exception)
			{ return {ETaskState::Failed, FailureDiagnostic(0, Num, Exception.what()), 1}; }
			catch (...)
			{ return {ETaskState::Failed, FailureDiagnostic(0, Num, "callable threw an unknown exception."), 1}; }
			if (Token.IsCancellationRequested())
				return {ETaskState::Canceled, "ParallelFor cancellation prevented full range coverage.", 1};
			return {ETaskState::Succeeded, {}, 1};
		}

		struct FSharedParallelForState
		{
			std::mutex Mutex;
			FTaskCancellationSource CancellationSource;
			F Function;
			explicit FSharedParallelForState(F&& InFunction) : Function(std::move(InFunction)) {}
			uint64 LowestFailedChunkStart = std::numeric_limits<uint64>::max();
			std::string FailureDiagnostic;
		};

		auto SharedState = std::make_shared<FSharedParallelForState>(std::move(Function));

		auto RecordFailure = [SharedState, FailureDiagnostic](uint64 ChunkStart, uint64 ChunkEnd, const char* Diagnostic) {
			std::lock_guard Lock(SharedState->Mutex);
			if (ChunkStart < SharedState->LowestFailedChunkStart)
			{
				SharedState->LowestFailedChunkStart = ChunkStart;
				SharedState->FailureDiagnostic = FailureDiagnostic(ChunkStart, ChunkEnd, Diagnostic);
			}
		};

		auto ExecuteChunk = [SharedState, ExternalToken, RecordFailure, Invoke](uint64 ChunkStart, uint64 ChunkEnd, const FTaskCancellationToken& TaskToken) {
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
					Invoke(SharedState->Function, Index, CancellationToken);
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
		for (uint32 ChunkIndex = 1; ChunkIndex < EffectiveChunkCount; ++ChunkIndex)
		{
			const auto [ChunkStart, ChunkEnd] = GetChunkRange(ChunkIndex);
			auto Task = Private::LaunchCancelableTaskWithCompletion(
				Name ? Name : "ParallelFor",
				[ExecuteChunk, ChunkStart, ChunkEnd](const FTaskCancellationToken& TaskToken) {
					ExecuteChunk(ChunkStart, ChunkEnd, TaskToken);
				}, {},
				LaunchOptions
			);
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

		const std::vector<FTaskWaitResult> WorkerOutcomes = WaitAll(WorkerTasks);
		bool bAnyCanceled = SharedState->CancellationSource.IsCancellationRequested() || Options.CancellationToken.IsCancellationRequested();
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
				"ParallelFor cancellation prevented full range coverage.",
				EffectiveChunkCount
			};
		}
		return {ETaskState::Succeeded, {}, EffectiveChunkCount};
	}
	auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		auto Body = std::move(Function);
		return Private::FParallelForAccess::Execute(Name, Num, std::move(Body), Options);
	}

	auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		return Private::FParallelForAccess::Execute(Name, Num, std::move(Function), Options);
	}

} // namespace Durin

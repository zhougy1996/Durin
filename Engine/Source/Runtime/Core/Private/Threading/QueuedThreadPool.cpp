#include "Threading/QueuedThreadPool.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Profiling/Profiling.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FQueuedWork
		{
			std::string Name;
			std::unique_ptr<FQueuedWorkFunction> Function;
			FQueuedWorkDiscardFunction Discard;
			EQueuedWorkPriority Priority = EQueuedWorkPriority::Normal;
			uint64 Sequence = 0;
		};
	}

	namespace Private
	{
		struct FQueuedWorkNode
		{
			FQueuedWorkNode(const void* InPool, FQueuedWork&& InWork) : Pool(InPool), Work(std::move(InWork)) {}
			const void* const Pool;
			FQueuedWork Work;
			// Links and membership are protected by the owning pool's mutex.
			FQueuedWorkNode* Previous = nullptr;
			std::shared_ptr<FQueuedWorkNode> Next;
			bool bQueued = false;
		};
	}

	namespace
	{
		// FIFO within each priority; every eighth dispatch selects the oldest head.
		// New interactive arrivals cannot indefinitely overtake previously queued work.
		struct FWorkQueue
		{
			struct FLane
			{
				std::shared_ptr<Private::FQueuedWorkNode> Head;
				Private::FQueuedWorkNode* Tail = nullptr;
				size_t Count = 0;
			};
			std::array<FLane, 3> Lanes;
			uint64 NextSequence = 0;
			uint32 PreferredDispatches = 0;
			FWorkQueue() = default;
			FWorkQueue(const FWorkQueue&) = delete;
			auto operator=(const FWorkQueue&) -> FWorkQueue& = delete;
			FWorkQueue(FWorkQueue&& Other) noexcept { Swap(Other); }
			auto operator=(FWorkQueue&& Other) noexcept -> FWorkQueue&
			{
				require(IsEmpty());
				Swap(Other);
				return *this;
			}
			~FWorkQueue()
			{
				// Detach iteratively so a long owning chain cannot overflow the stack.
				while (!IsEmpty()) { auto Work = Take(); }
			}
			auto Swap(FWorkQueue& Other) noexcept -> void
			{
				Lanes.swap(Other.Lanes);
				std::swap(NextSequence, Other.NextSequence);
				std::swap(PreferredDispatches, Other.PreferredDispatches);
			}
			auto IsEmpty() const -> bool { return Num() == 0; }
			auto Num() const -> size_t { return Lanes[0].Count + Lanes[1].Count + Lanes[2].Count; }
			auto Enqueue(const std::shared_ptr<Private::FQueuedWorkNode>& Node) -> void
			{
				auto& Lane = Lanes[static_cast<size_t>(Node->Work.Priority)];
				Node->Previous = Lane.Tail;
				if (Lane.Tail) Lane.Tail->Next = Node;
				else Lane.Head = Node;
				Lane.Tail = Node.get();
				++Lane.Count;
				Node->bQueued = true;
				Node->Work.Sequence = NextSequence++;
			}
			auto Remove(const std::shared_ptr<Private::FQueuedWorkNode>& Node, FQueuedWork& OutWork) -> bool
			{
				if (!Node->bQueued) return false;
				OutWork = std::move(Node->Work);
				auto& Lane = Lanes[static_cast<size_t>(OutWork.Priority)];
				auto Next = std::move(Node->Next);
				if (Node->Previous) Node->Previous->Next = Next;
				else Lane.Head = Next;
				if (Next) Next->Previous = Node->Previous;
				else Lane.Tail = Node->Previous;
				Node->Previous = nullptr;
				Node->bQueued = false;
				--Lane.Count;
				return true;
			}
			auto Take() -> FQueuedWork
			{
				size_t Selected = 3;
				for (size_t Index = 0; Index < 3; ++Index)
				{
					if (!Lanes[Index].Head) continue;
					if (Selected == 3 || (PreferredDispatches == 7 && Lanes[Index].Head->Work.Sequence < Lanes[Selected].Head->Work.Sequence)) Selected = Index;
				}
				require(Selected < 3);
				PreferredDispatches = (PreferredDispatches + 1) % 8;
				auto Node = Lanes[Selected].Head;
				FQueuedWork Work;
				const bool bRemoved = Remove(Node, Work);
				require(bRemoved);
				return Work;
			}
		};

		std::atomic<bool> GFailQueuedWorkAllocation = false;

		thread_local const void* GCurrentQueuedThreadPool = nullptr;
	}

	class FQueuedThreadPool::FImpl
	{
	public:
		class FWorkerRunnable final : public FRunnable
		{
		public:
			explicit FWorkerRunnable(FImpl& InPool)
				: Pool(InPool)
			{
			}

			auto Run() -> uint32 override
			{
				Pool.RunWorkerLoop();
				return 0;
			}

			auto Stop() -> void override
			{
				Pool.RequestStop(false);
			}

		private:
			FImpl& Pool;
		};

		~FImpl()
		{
			Destroy(false);
		}

		auto Create(uint32 InNumThreads, const char* InPoolName, uint32 WorkerCreationFailureIndex) -> bool
		{
			FWorkQueue PreviousWork;
			if (InNumThreads == 0)
			{
				DURIN_WARN("Queued thread pool creation failed because thread count is zero. (pool: {})", InPoolName ? InPoolName : "");
				return false;
			}

			{
				std::lock_guard Lock(Mutex);
				if (bRunning)
				{
					DURIN_WARN("Queued thread pool creation ignored because the pool is already running. (pool: {})", PoolName);
					return false;
				}

				PoolName = InPoolName ? InPoolName : "WorkerPool";
				bRunning = true;
				bAcceptingWork = true;
				bStopRequested = false;
				bDrainQueuedWorkOnStop = true;
				ActiveTaskCount = 0;
				PreviousWork = std::move(Queue);

				WorkerRunnables.reserve(InNumThreads);
				WorkerThreads.reserve(InNumThreads);
				for (uint32 WorkerIndex = 0; WorkerIndex < InNumThreads; ++WorkerIndex)
				{
					auto Runnable = std::make_unique<FWorkerRunnable>(*this);
					std::string WorkerName = PoolName + "-" + std::to_string(WorkerIndex);
					FRunnableThread* Thread = WorkerIndex == WorkerCreationFailureIndex
						? nullptr
						: FRunnableThread::Create(Runnable.get(), WorkerName.c_str(), 0, EThreadPriority::Normal, EThreadRole::WorkerThread);
					if (!Thread)
					{
						DURIN_WARN("Queued thread pool worker creation failed. (pool: {}, worker: {})", PoolName, WorkerIndex);
						bAcceptingWork = false;
						bStopRequested = true;
						bDrainQueuedWorkOnStop = false;
						break;
					}

					WorkerRunnables.emplace_back(std::move(Runnable));
					WorkerThreads.emplace_back(Thread);
				}
			}
			DiscardWork(std::move(PreviousWork));

			if (GetNumThreads() != InNumThreads)
			{
				Destroy(false);
				return false;
			}

			DURIN_DEBUG("Queued thread pool created. (pool: {}, workers: {})", PoolName, WorkerThreads.size());
			return true;
		}

		auto Destroy(bool bWaitForQueuedWork) -> void
		{
			std::vector<std::unique_ptr<FRunnableThread>> ThreadsToJoin;
			std::vector<std::unique_ptr<FWorkerRunnable>> RunnablesToDestroy;
			FWorkQueue DiscardedWork;
			std::string DestroyedPoolName;
			size_t DestroyedWorkerCount = 0;

			{
				std::lock_guard Lock(Mutex);
				if (!bRunning && WorkerThreads.empty())
				{
					return;
				}

				DestroyedPoolName = PoolName;
				DestroyedWorkerCount = WorkerThreads.size();
				bAcceptingWork = false;
				bStopRequested = true;
				bDrainQueuedWorkOnStop = bWaitForQueuedWork;
				if (!bWaitForQueuedWork)
				{
					DiscardedWork = std::move(Queue);
					NotifyIdleIfNeeded();
				}

				ThreadsToJoin = std::move(WorkerThreads);
				RunnablesToDestroy = std::move(WorkerRunnables);
			}

			DiscardWork(std::move(DiscardedWork));

			WorkAvailableCV.notify_all();

			for (std::unique_ptr<FRunnableThread>& Thread : ThreadsToJoin)
			{
				if (Thread)
				{
					Thread->WaitForCompletion();
				}
			}

			{
				std::lock_guard Lock(Mutex);
				bRunning = false;
				bStopRequested = false;
				bDrainQueuedWorkOnStop = true;
				NotifyIdleIfNeeded();
			}

			DURIN_DEBUG("Queued thread pool destroyed. (pool: {}, workers: {}, drained: {})", DestroyedPoolName, DestroyedWorkerCount, bWaitForQueuedWork);
		}

		auto StopAcceptingWork() -> void
		{
			std::lock_guard Lock(Mutex);
			bAcceptingWork = false;
		}

		auto Enqueue(
			const char* TaskName,
			FQueuedWorkFunction&& Work,
			FQueuedWorkDiscardFunction&& Discard,
			EQueuedWorkPriority Priority,
			FQueuedWorkHandle* OutHandle
		) -> bool
		{
			if (OutHandle) *OutHandle = {};
			if (!Work || static_cast<uint8>(Priority) > static_cast<uint8>(EQueuedWorkPriority::Low))
			{
				return false;
			}
			// Keep all user storage outside the lock scope until queue insertion succeeds.
			FQueuedWork Item{TaskName ? TaskName : "QueuedWork",
				std::make_unique<FQueuedWorkFunction>(std::move(Work)), std::move(Discard), Priority};
			auto Node = std::make_shared<Private::FQueuedWorkNode>(this, std::move(Item));
			{
				std::lock_guard Lock(Mutex);
				if (!bRunning || !bAcceptingWork || bStopRequested) return false;
				try
				{
					if (GFailQueuedWorkAllocation.exchange(false)) throw std::bad_alloc();
					Queue.Enqueue(Node);
					if (OutHandle) OutHandle->Node = Node;
				}
				catch (...)
				{
					throw;
				}
			}

			WorkAvailableCV.notify_one();
			return true;
		}

		auto TryExecuteQueuedTask(const FQueuedWorkHandle& Handle) -> bool
		{
			auto Node = Handle.Node.lock();
			if (!Node || Node->Pool != this) return false;
			FQueuedWork Work;
			{
				std::lock_guard Lock(Mutex);
				if (bStopRequested && !bDrainQueuedWorkOnStop) return false;
				if (!Queue.Remove(Node, Work)) return false;
				++ActiveTaskCount;
			}
			const auto PreviousPool = GCurrentQueuedThreadPool;
			GCurrentQueuedThreadPool = this;
			ExecuteQueuedWork(std::move(Work));
			GCurrentQueuedThreadPool = PreviousPool;
			return true;
		}

		auto TryExecuteOneQueuedTask() -> bool
		{
			FQueuedWork Work;
			if (!TryDequeueWork(Work))
			{
				return false;
			}

			ExecuteQueuedWork(std::move(Work));
			return true;
		}





		auto WaitForIdle() -> bool
		{
			if (GCurrentQueuedThreadPool == this)
			{
				DURIN_WARN("Queued thread pool idle wait rejected from one of its workers. (pool: {})", PoolName);
				return false;
			}

			std::unique_lock Lock(Mutex);
			IdleCV.wait(Lock, [this]() {
				return Queue.IsEmpty() && ActiveTaskCount == 0;
			});
			return true;
		}

		auto GetNumThreads() const -> uint32
		{
			std::lock_guard Lock(Mutex);
			return static_cast<uint32>(WorkerThreads.size());
		}

		auto GetNumQueuedTasks() const -> uint32
		{
			std::lock_guard Lock(Mutex);
			return static_cast<uint32>(Queue.Num());
		}

		auto IsRunning() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bRunning && bAcceptingWork && !bStopRequested;
		}

	private:
		auto RequestStop(bool bWaitForQueuedWork) -> void
		{
			FWorkQueue DiscardedWork;
			{
				std::lock_guard Lock(Mutex);
				bAcceptingWork = false;
				bStopRequested = true;
				bDrainQueuedWorkOnStop = bWaitForQueuedWork;
				if (!bWaitForQueuedWork)
				{
					DiscardedWork = std::move(Queue);
					NotifyIdleIfNeeded();
				}
			}
			DiscardWork(std::move(DiscardedWork));

			WorkAvailableCV.notify_all();
		}

		auto RunWorkerLoop() -> void
		{
			const void* PreviousPool = GCurrentQueuedThreadPool;
			GCurrentQueuedThreadPool = this;
			while (true)
			{
				FQueuedWork Work;
				{
					std::unique_lock Lock(Mutex);
					WorkAvailableCV.wait(Lock, [this]() {
						return bStopRequested || !Queue.IsEmpty();
					});

					if (bStopRequested && (!bDrainQueuedWorkOnStop || Queue.IsEmpty()))
					{
						break;
					}

					if (Queue.IsEmpty())
					{
						continue;
					}

					Work = Queue.Take();
					++ActiveTaskCount;
				}

				ExecuteQueuedWork(std::move(Work));
			}
			GCurrentQueuedThreadPool = PreviousPool;
		}

		auto TryDequeueWork(FQueuedWork& OutWork) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (Queue.IsEmpty())
			{
				return false;
			}

			if (bStopRequested && !bDrainQueuedWorkOnStop)
			{
				return false;
			}

			OutWork = Queue.Take();
			++ActiveTaskCount;
			return true;
		}

		auto ExecuteQueuedWork(FQueuedWork&& Work) -> void
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("QueuedTask.Execute");
			DURIN_TRACE("Queued task started. (task: {}, thread: {}, id: {})", Work.Name, GetCurrentThreadName(), FPlatformLTS::GetCurrentThreadId());
			try
			{
				(*Work.Function)();
			}
			catch (const std::exception& Exception)
			{
				DURIN_ERROR("Queued task escaped its callable boundary. (task: {}, error: {})", Work.Name, Exception.what());
			}
			catch (...)
			{
				DURIN_ERROR("Queued task escaped its callable boundary with an unknown exception. (task: {})", Work.Name);
			}
			DURIN_TRACE("Queued task finished. (task: {}, thread: {}, id: {})", Work.Name, GetCurrentThreadName(), FPlatformLTS::GetCurrentThreadId());
			Work.Function.reset();
			Work.Discard = {};

			{
				std::lock_guard Lock(Mutex);
				--ActiveTaskCount;
				NotifyIdleIfNeeded();
			}
		}

		auto DiscardWork(FWorkQueue&& WorkItems) -> void
		{
			while (!WorkItems.IsEmpty())
			{
				auto Work = WorkItems.Take();
				if (Work.Discard)
				{
					try
					{
						Work.Discard();
					}
					catch (const std::exception& Exception)
					{
						DURIN_ERROR("Queued task discard callback failed. (task: {}, error: {})", Work.Name, Exception.what());
					}
					catch (...)
					{
						DURIN_ERROR("Queued task discard callback failed with an unknown exception. (task: {})", Work.Name);
					}
				}
				Work.Function.reset();
				Work.Discard = {};
				std::lock_guard Lock(Mutex);
			}
		}



		auto NotifyIdleIfNeeded() -> void
		{
			if (Queue.IsEmpty() && ActiveTaskCount == 0)
			{
				IdleCV.notify_all();
			}
		}

		mutable std::mutex Mutex;
		std::condition_variable WorkAvailableCV;
		std::condition_variable IdleCV;
		FWorkQueue Queue;

		std::vector<std::unique_ptr<FWorkerRunnable>> WorkerRunnables;
		std::vector<std::unique_ptr<FRunnableThread>> WorkerThreads;

		std::string PoolName = "WorkerPool";
		uint32 ActiveTaskCount = 0;
		bool bRunning = false;
		bool bAcceptingWork = false;
		bool bStopRequested = false;
		bool bDrainQueuedWorkOnStop = true;
	};

	FQueuedThreadPool::FQueuedThreadPool()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FQueuedThreadPool::~FQueuedThreadPool() = default;

	auto FQueuedThreadPool::Create(uint32 InNumThreads, const char* InPoolName, uint32 WorkerCreationFailureIndex) -> bool
	{
		return Impl->Create(InNumThreads, InPoolName, WorkerCreationFailureIndex);
	}

	auto FQueuedThreadPool::Destroy(bool bWaitForQueuedWork) -> void
	{
		Impl->Destroy(bWaitForQueuedWork);
	}

	auto FQueuedThreadPool::StopAcceptingWork() -> void
	{
		Impl->StopAcceptingWork();
	}

	auto FQueuedThreadPool::Enqueue(
		const char* TaskName,
		FQueuedWorkFunction&& Work,
		FQueuedWorkDiscardFunction&& Discard,
		EQueuedWorkPriority Priority,
		FQueuedWorkHandle* OutHandle
	) -> bool
	{
		return Impl->Enqueue(TaskName, std::move(Work), std::move(Discard), Priority, OutHandle);
	}

	auto FQueuedThreadPool::TryExecuteQueuedTask(const FQueuedWorkHandle& Handle) -> bool
	{ return Impl->TryExecuteQueuedTask(Handle); }

	auto FQueuedThreadPool::TryExecuteOneQueuedTask() -> bool
	{
		return Impl->TryExecuteOneQueuedTask();
	}





	auto FQueuedThreadPool::WaitForIdle() -> bool
	{
		return Impl->WaitForIdle();
	}

	auto FQueuedThreadPool::GetNumThreads() const -> uint32
	{
		return Impl->GetNumThreads();
	}

	auto FQueuedThreadPool::GetNumQueuedTasks() const -> uint32
	{
		return Impl->GetNumQueuedTasks();
	}

	auto FQueuedThreadPool::IsRunning() const -> bool
	{
		return Impl->IsRunning();
	}

	auto Private::SetQueuedWorkAllocationFailureForTests(bool bFail) -> void
	{
		GFailQueuedWorkAllocation.store(bFail);
	}

	auto GetDefaultThreadPoolThreadCount() -> uint32
	{
		const uint32 HardwareThreadCount = std::thread::hardware_concurrency();
		if (HardwareThreadCount <= 2)
		{
			return 1;
		}

		return HardwareThreadCount - 2;
	}

}

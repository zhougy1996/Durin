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
			uint64 OwnerTag = 0;
		};

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
			std::deque<FQueuedWork> PreviousWork;
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
			std::deque<FQueuedWork> DiscardedWork;
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
			uint64 OwnerTag
		) -> bool
		{
			if (!Work)
			{
				return false;
			}
			auto WorkOwner = std::make_unique<FQueuedWorkFunction>(std::move(Work));

			{
				std::lock_guard Lock(Mutex);
				if (!bRunning || !bAcceptingWork || bStopRequested)
				{
					return false;
				}

				Queue.emplace_back(FQueuedWork{
					TaskName ? TaskName : "QueuedWork",
					std::move(WorkOwner),
					std::move(Discard),
					OwnerTag,
				});
				if (OwnerTag != 0) ++OutstandingOwnerTags[OwnerTag];
			}

			WorkAvailableCV.notify_one();
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

		auto WaitForOwnerTagIdle(uint64 OwnerTag, double TimeoutSeconds) -> bool
		{
			if (OwnerTag == 0) return true;
			if (GCurrentQueuedThreadPool == this) return false;
			std::unique_lock Lock(Mutex);
			return OwnerIdleCV.wait_for(Lock, std::chrono::duration<double>(std::max(0.0, TimeoutSeconds)), [&]() {
				return !OutstandingOwnerTags.contains(OwnerTag);
			});
		}

		auto GetOwnerTagOutstandingCount(uint64 OwnerTag) const -> uint32
		{
			if (OwnerTag == 0) return 0;
			std::lock_guard Lock(Mutex);
			const auto Iterator = OutstandingOwnerTags.find(OwnerTag);
			return Iterator == OutstandingOwnerTags.end() ? 0u : Iterator->second;
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
				return Queue.empty() && ActiveTaskCount == 0;
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
			return static_cast<uint32>(Queue.size());
		}

		auto IsRunning() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bRunning && bAcceptingWork && !bStopRequested;
		}

	private:
		auto RequestStop(bool bWaitForQueuedWork) -> void
		{
			std::deque<FQueuedWork> DiscardedWork;
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
						return bStopRequested || !Queue.empty();
					});

					if (bStopRequested && (!bDrainQueuedWorkOnStop || Queue.empty()))
					{
						break;
					}

					if (Queue.empty())
					{
						continue;
					}

					Work = std::move(Queue.front());
					Queue.pop_front();
					++ActiveTaskCount;
				}

				ExecuteQueuedWork(std::move(Work));
			}
			GCurrentQueuedThreadPool = PreviousPool;
		}

		auto TryDequeueWork(FQueuedWork& OutWork) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (Queue.empty())
			{
				return false;
			}

			if (bStopRequested && !bDrainQueuedWorkOnStop)
			{
				return false;
			}

			OutWork = std::move(Queue.front());
			Queue.pop_front();
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
				ReleaseOwnerTagLocked(Work.OwnerTag);
				NotifyIdleIfNeeded();
			}
		}

		auto DiscardWork(std::deque<FQueuedWork>&& WorkItems) -> void
		{
			for (FQueuedWork& Work : WorkItems)
			{
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
				ReleaseOwnerTagLocked(Work.OwnerTag);
			}
		}

		auto ReleaseOwnerTagLocked(uint64 OwnerTag) -> void
		{
			if (OwnerTag == 0) return;
			auto Iterator = OutstandingOwnerTags.find(OwnerTag);
			require(Iterator != OutstandingOwnerTags.end() && Iterator->second > 0);
			if (--Iterator->second == 0)
			{
				OutstandingOwnerTags.erase(Iterator);
				OwnerIdleCV.notify_all();
			}
		}

		auto NotifyIdleIfNeeded() -> void
		{
			if (Queue.empty() && ActiveTaskCount == 0)
			{
				IdleCV.notify_all();
			}
		}

		mutable std::mutex Mutex;
		std::condition_variable WorkAvailableCV;
		std::condition_variable IdleCV;
		std::condition_variable OwnerIdleCV;
		std::deque<FQueuedWork> Queue;
		std::unordered_map<uint64, uint32> OutstandingOwnerTags;

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
		uint64 OwnerTag
	) -> bool
	{
		return Impl->Enqueue(TaskName, std::move(Work), std::move(Discard), OwnerTag);
	}

	auto FQueuedThreadPool::TryExecuteOneQueuedTask() -> bool
	{
		return Impl->TryExecuteOneQueuedTask();
	}

	auto FQueuedThreadPool::WaitForOwnerTagIdle(uint64 OwnerTag, double TimeoutSeconds) -> bool
	{
		return Impl->WaitForOwnerTagIdle(OwnerTag, TimeoutSeconds);
	}

	auto FQueuedThreadPool::GetOwnerTagOutstandingCount(uint64 OwnerTag) const -> uint32
	{
		return Impl->GetOwnerTagOutstandingCount(OwnerTag);
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

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
			FQueuedWorkFunction Function;
		};

		std::mutex GThreadPoolMutex;
		std::unique_ptr<FQueuedThreadPool> GThreadPoolStorage;
	}

	FQueuedThreadPool* GThreadPool = nullptr;

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

		auto Create(uint32 InNumThreads, const char* InPoolName) -> bool
		{
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
				Queue.clear();

				WorkerRunnables.reserve(InNumThreads);
				WorkerThreads.reserve(InNumThreads);
				for (uint32 WorkerIndex = 0; WorkerIndex < InNumThreads; ++WorkerIndex)
				{
					auto Runnable = std::make_unique<FWorkerRunnable>(*this);
					std::string WorkerName = PoolName + "-" + std::to_string(WorkerIndex);
					FRunnableThread* Thread = FRunnableThread::Create(Runnable.get(), WorkerName.c_str(), 0, EThreadPriority::Normal, EThreadRole::WorkerThread);
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
					Queue.clear();
					NotifyIdleIfNeeded();
				}

				ThreadsToJoin = std::move(WorkerThreads);
				RunnablesToDestroy = std::move(WorkerRunnables);
			}

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

		auto Enqueue(const char* TaskName, FQueuedWorkFunction&& Work) -> bool
		{
			if (!Work)
			{
				return false;
			}

			{
				std::lock_guard Lock(Mutex);
				if (!bRunning || !bAcceptingWork || bStopRequested)
				{
					return false;
				}

				Queue.emplace_back(FQueuedWork{
					TaskName ? TaskName : "QueuedWork",
					std::move(Work),
				});
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

		auto WaitForIdle() -> void
		{
			std::unique_lock Lock(Mutex);
			IdleCV.wait(Lock, [this]() {
				return Queue.empty() && ActiveTaskCount == 0;
			});
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
			{
				std::lock_guard Lock(Mutex);
				bAcceptingWork = false;
				bStopRequested = true;
				bDrainQueuedWorkOnStop = bWaitForQueuedWork;
				if (!bWaitForQueuedWork)
				{
					Queue.clear();
					NotifyIdleIfNeeded();
				}
			}

			WorkAvailableCV.notify_all();
		}

		auto RunWorkerLoop() -> void
		{
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
			Work.Function();
			DURIN_TRACE("Queued task finished. (task: {}, thread: {}, id: {})", Work.Name, GetCurrentThreadName(), FPlatformLTS::GetCurrentThreadId());

			{
				std::lock_guard Lock(Mutex);
				--ActiveTaskCount;
				NotifyIdleIfNeeded();
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
		std::deque<FQueuedWork> Queue;

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

	auto FQueuedThreadPool::Create(uint32 InNumThreads, const char* InPoolName) -> bool
	{
		return Impl->Create(InNumThreads, InPoolName);
	}

	auto FQueuedThreadPool::Destroy(bool bWaitForQueuedWork) -> void
	{
		Impl->Destroy(bWaitForQueuedWork);
	}

	auto FQueuedThreadPool::StopAcceptingWork() -> void
	{
		Impl->StopAcceptingWork();
	}

	auto FQueuedThreadPool::Enqueue(const char* TaskName, FQueuedWorkFunction&& Work) -> bool
	{
		return Impl->Enqueue(TaskName, std::move(Work));
	}

	auto FQueuedThreadPool::TryExecuteOneQueuedTask() -> bool
	{
		return Impl->TryExecuteOneQueuedTask();
	}

	auto FQueuedThreadPool::WaitForIdle() -> void
	{
		Impl->WaitForIdle();
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

	auto InitEngineThreadPool(uint32 InNumThreads) -> bool
	{
		std::lock_guard Lock(GThreadPoolMutex);
		if (GThreadPool && GThreadPool->IsRunning())
		{
			DURIN_WARN("Engine thread pool initialization ignored because it is already running. (workers: {})", GThreadPool->GetNumThreads());
			return true;
		}

		const uint32 NumThreads = InNumThreads > 0 ? InNumThreads : GetDefaultThreadPoolThreadCount();
		GThreadPoolStorage = std::make_unique<FQueuedThreadPool>();
		if (!GThreadPoolStorage->Create(NumThreads, "EngineWorker"))
		{
			GThreadPoolStorage.reset();
			GThreadPool = nullptr;
			return false;
		}

		GThreadPool = GThreadPoolStorage.get();
		DURIN_DEBUG("Engine thread pool initialized. (workers: {})", GThreadPool->GetNumThreads());
		return true;
	}

	auto ShutdownEngineThreadPool(bool bWaitForQueuedWork) -> void
	{
		std::unique_ptr<FQueuedThreadPool> ThreadPoolToDestroy;
		{
			std::lock_guard Lock(GThreadPoolMutex);
			ThreadPoolToDestroy = std::move(GThreadPoolStorage);
			GThreadPool = nullptr;
		}

		if (ThreadPoolToDestroy)
		{
			ThreadPoolToDestroy->Destroy(bWaitForQueuedWork);
			DURIN_DEBUG("Engine thread pool shut down. (drained: {})", bWaitForQueuedWork);
		}
	}
}

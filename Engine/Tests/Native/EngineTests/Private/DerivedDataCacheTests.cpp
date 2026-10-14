#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "DerivedDataCache/DerivedDataCacheAsync.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/DerivedDataCacheStorage.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/CompressedCacheValue.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "../../../../Source/Runtime/Engine/Private/Asset/AssetCompilationOperation.h"
#include "../../../../Source/Runtime/Engine/Private/Asset/ResourceBuildSessions.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryFormat.h"
#include "Threading/TaskComposition.h"
#include "Threading/ThreadEvent.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"

#include <latch>
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#endif

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto EnsureCacheTaskGameThread() -> void
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
	}
	static_assert(sizeof(FCacheBucket) == sizeof(void*));
	static_assert(sizeof(FCacheKey) == 24);
	static_assert(std::is_trivially_copyable_v<FCacheKey>);
	static_assert(sizeof(FCacheKeyProxy) == sizeof(FCacheKey));

	class FScopedCacheDirectory
	{
	public:
		explicit FScopedCacheDirectory(std::string_view Name)
			: Previous(FPaths::DerivedDataCacheDir())
			, Root(Testing::GetTestWorkDirectory() / Name)
		{
			Testing::RemoveTestWorkDirectory(Root);
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedCacheDirectory()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}

		std::string Previous;
		std::filesystem::path Root;
	};

	auto MakeKey(const FCacheBucket& Bucket, char Fill) -> FCacheKey
	{
		return FCacheKey::FromString(Bucket, std::string(32, Fill));
	}

	auto Bytes(std::initializer_list<uint8> Values) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}
	auto RecordWith(const FCacheKey& Key, std::initializer_list<FSharedByteBuffer> Blocks) -> FCacheRecord
	{
		FBuildOutputBuilder Builder("Fixture.Output", 1);
		uint32 Index = 0;
		for (const auto& Block : Blocks) Builder.AddValue(FValueId::FromName("Block").MakeIndexed(Index++), Block);
		Builder.AddMeta(FValueId::FromName("Metadata"), MakeBuildMetadata(FSharedByteBuffer::Take(Bytes({7, 8}))).value());
		Builder.AddMessage(EBuildMessageSeverity::Warning, "fixture warning");
		return FCacheRecord::FromOutput(Key, std::move(Builder).Build().value()).value();
	}
	auto BlobKey(const FValue& Value) -> FCacheKey
	{ return FCacheKey::FromHash(FCacheBucket::FromString("Content"), Value.GetRawHash()); }
	auto EntryPath(const FScopedCacheDirectory& Directory, const FCacheKey& Key, std::string_view Namespace) -> std::filesystem::path
	{ return Directory.Root / Namespace / Key.GetBucket().ToString() / Key.ToString().substr(0, 2) / (Key.ToString() + ".bin"); }
}

namespace
{
	class FCacheQueueScheduler final : public IBuildScheduler
	{
	public:
		std::deque<std::function<void()>> Work;
		std::vector<ERequestPriority> Priorities;
		bool Reject = false;
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Task)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			if (Reject) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, "test rejection"});
			Priorities.push_back(Params.Priority); Work.push_back(std::move(Task)); return std::shared_ptr<IBuildScheduledWork>{};
		}
		auto Run() -> void { auto Task = std::move(Work.front()); Work.pop_front(); Task(); }
	};
	class FAsyncFixtureCache final : public ICache
	{
	public:
		mutable uint32 Gets = 0, Puts = 0;
		mutable std::vector<FValueId> ObservedIds;
		bool Throw = false;
		auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult override
		{
			++Gets; if (Throw) throw std::runtime_error("fixture");
			ObservedIds.assign(Request.RequiredValues.begin(), Request.RequiredValues.end());
			if (Request.Stats) Request.Stats->ReadBytes = 17;
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
		{ ++Puts; return Request.Record.ToOutput(Request.Record.GetKey()).transform([](auto&&) {}); }
	};
}

TEST(FAsyncCacheTests, CanceledQueuedOwnerCompletesWithoutOtherOwnerProgress)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner First, Second;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '1')}};
	uint32 Calls = 0;
	ASSERT_TRUE(Async->Get(Requests, First, [](auto, auto, auto) {}));
	auto Batch = Async->Get(Requests, Second, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Calls; });
	ASSERT_TRUE(Batch);
	Second.Cancel();
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Second.Poll());
	EXPECT_EQ(Second.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ((*Batch)->Wait(), ERequestWaitResult::Completed);
	EXPECT_FALSE(First.Poll());
	ASSERT_EQ(Scheduler->Work.size(), 1u);
	Scheduler->Run();
	EXPECT_EQ(Calls, 1u); EXPECT_EQ(Backend->Gets, 1u);
}

TEST(FAsyncCacheTests, AlreadyCanceledOwnerCompletesWithoutTaskDispatch)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Owner; Owner.Cancel();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '2')}};
	uint32 Calls = 0;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; }));
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Scheduler->Work.empty()); EXPECT_TRUE(Owner.Poll());
}

TEST(FAsyncCacheTests, ScopeDoesNotCancelSharedOwnerAndBatchCancellationStaysIndependent)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>(); auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner Busy, Owner; Tasks::FTaskGroup Scope;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("ScopeCancel"), '5')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	std::atomic<uint32> Canceled = 0; uint32 Other = 0;
	std::shared_ptr<IBuildScheduledWork> Batch;
	auto Submit = Tasks::LaunchTask(Scope, Tasks::ETaskExecutor::Worker, {}, [&] {
		auto Accepted = Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Canceled; });
		EXPECT_TRUE(Accepted); if (Accepted) Batch = *Accepted;
	});
	EXPECT_EQ(Submit.Wait().WaitStatus, ETaskWaitStatus::Completed);
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Other; }));
	EXPECT_EQ(Scope.Close(ETaskScopeCloseMode::Cancel), ETaskScopeCloseResult::Closed);
	EXPECT_EQ(Scope.WaitFor(5.0), ETaskScopeWaitResult::Quiescent); EXPECT_EQ(Canceled.load(), 0u);
	ASSERT_TRUE(Batch); Batch->RequestCancel(); EXPECT_EQ(Batch->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Canceled.load(), 1u);
	EXPECT_FALSE(Owner.IsCanceled()); EXPECT_FALSE(Owner.Poll()); EXPECT_EQ(Other, 0u); EXPECT_FALSE(Busy.Poll());
	Scheduler->Run(); Scheduler->Run(); EXPECT_EQ(Other, 1u); EXPECT_TRUE(Owner.Poll());
}

TEST(FAsyncCacheTests, OwnerDestructionCancelsQueuedBatchBeforeReturning)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '3')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	uint32 Calls = 0;
	{ FRequestOwner Owner; ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; })); }
	EXPECT_EQ(Calls, 1u); EXPECT_FALSE(Busy.Poll()); Scheduler->Run();
}

TEST(FAsyncCacheTests, OwnerKeepsQueuedRequestCountedWhileScopeDrains)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FBlockingCache final : public ICache
	{
	public:
		std::promise<void> Entered, Release;
		std::shared_future<void> Gate = Release.get_future().share();
		mutable std::atomic<uint32> Gets = 0;
		auto Get(const FCacheGetRequest&) const -> FCacheGetResult override
		{ if (++Gets == 1) { const_cast<FBlockingCache*>(this)->Entered.set_value(); Gate.wait(); } return std::optional<FCacheRecord>{}; }
		auto Put(const FCachePutRequest&) const -> FCachePutResult override { return {}; }
	};
	auto Backend = std::make_shared<FBlockingCache>();
	auto Entered = Backend->Entered.get_future(); auto Async = CreateAsyncCache(Backend, {}, {.Concurrency = 1});
	FRequestOwner Busy; auto Operation = std::make_shared<FAssetCompilationOperation>(); auto& Queued = *Operation; Tasks::FTaskGroup Scope;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("ScopeLease"), '4')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	const auto Started = Entered.wait_for(std::chrono::seconds(5));
	std::atomic<uint32> Calls = 0;
	std::promise<void> Submitted; auto SubmittedFuture = Submitted.get_future();
	auto Submit = Tasks::LaunchTask(Scope, Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Requests, Queued, [&](auto, auto, auto) { EXPECT_EQ(Durin::Private::FTaskRuntimeAccess::GetCurrentScope(), FTaskScopeToken{}); ++Calls; }));
		Tasks::AddNested(Queued.GetCompletionEvent()); Queued.CloseSubmission(); Submitted.set_value();
	});
	EXPECT_EQ(SubmittedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	EXPECT_FALSE(Submit.IsCompleted());
	EXPECT_EQ(Scope.Close(), ETaskScopeCloseResult::Closed);
	// Group waits on GameThread reject pending work; completion stays pending until DDC retires.
	EXPECT_EQ(Scope.WaitFor(0.01), ETaskScopeWaitResult::UnsupportedThread);
	Backend->Release.set_value(); EXPECT_EQ(Started, std::future_status::ready);
	EXPECT_EQ(Queued.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Busy.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Submit.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Scope.WaitFor(5.0), ETaskScopeWaitResult::Quiescent); EXPECT_EQ(Calls.load(), 1u);
}

TEST(FAsyncCacheTests, AssetOperationIncludesNativeDispatchCaptureRetirement)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FTailScheduler final : public IBuildScheduler
	{
	public:
		class FWork final : public IBuildScheduledWork
		{
		public:
			FTaskHandle Handle;
			auto GetTaskHandle() const -> FTaskHandle override { return Handle; }
			auto RequestCancel() const -> void override { CancelTask(Handle); }
			auto Wait() const -> ERequestWaitResult override
			{ return WaitTask(Handle).WaitStatus == ETaskWaitStatus::Completed ? ERequestWaitResult::Completed : ERequestWaitResult::WouldBlock; }
		};
		std::shared_ptr<int> Capture = std::make_shared<int>(42);
		FThreadEvent Entered, Release;
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			auto Scheduled = std::make_shared<FWork>();
			FTaskLaunchOptions Options; Options.bUseExplicitScope = true;
			auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion("DDCNativeTail",
				[this, Work = std::move(Work), Capture = std::exchange(Capture, {})](const FTaskCancellationToken&) {
					Work(); Entered.Trigger(); Release.Wait();
				}, {}, Options);
			if (!Admission.HasValue()) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, "test native dispatch rejected"});
			Scheduled->Handle = std::move(Admission).TakeValue(); return Scheduled;
		}
	};
	auto Scheduler = std::make_shared<FTailScheduler>(); std::weak_ptr<int> Weak = Scheduler->Capture;
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	auto Operation = std::make_shared<FAssetCompilationOperation>(); auto& Owner = *Operation; Tasks::FTaskGroup Scope; std::atomic<uint32> Calls = 0;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("NativeTail"), '6')}};
	FThreadEvent Submitted, WaitStarted, WaitReturned;
	auto Producer = Tasks::LaunchTask(Scope, Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Calls; }));
		Tasks::AddNested(Owner.GetCompletionEvent()); Owner.CloseSubmission(); Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_TRUE(Scheduler->Entered.WaitFor(5.0));
	EXPECT_EQ(Calls.load(), 1u); EXPECT_FALSE(Owner.Poll()); EXPECT_FALSE(Weak.expired());
	EXPECT_FALSE(Owner.GetCompletionEvent().IsCompleted()); EXPECT_FALSE(Producer.IsCompleted());
	EXPECT_EQ(Scope.Close(), ETaskScopeCloseResult::Closed);
	EXPECT_EQ(Scope.WaitFor(0.01), ETaskScopeWaitResult::UnsupportedThread);
	auto Waiter = Tasks::LaunchTask("DDCOwnerJoin", [&] {
		WaitStarted.Trigger(); EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); WaitReturned.Trigger();
	});
	EXPECT_TRUE(WaitStarted.WaitFor(5.0)); EXPECT_FALSE(WaitReturned.WaitFor(0.01));
	Scheduler->Release.Trigger();
	EXPECT_EQ(Waiter.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Scope.WaitFor(5.0), ETaskScopeWaitResult::Quiescent);
	EXPECT_TRUE(Owner.Poll()); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(WaitReturned.WaitFor(1.0));
}

TEST(FAsyncCacheTests, AssetOperationQueuedCancellationNeedsNoBusyOwnerProgress)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>(); auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner Busy; auto Operation = std::make_shared<FAssetCompilationOperation>(); auto& Owner = *Operation; Tasks::FTaskGroup Scope;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("NestedCancel"), '7')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted; std::atomic<uint32> Calls = 0;
	auto Producer = Tasks::LaunchTask(Scope, Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) {
			EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Calls;
		}));
		Tasks::AddNested(Owner.GetCompletionEvent()); Owner.CloseSubmission(); Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_FALSE(Producer.IsCompleted());
	Owner.Cancel(); EXPECT_EQ(Calls.load(), 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_FALSE(Busy.Poll());
	EXPECT_EQ(Scope.Close(ETaskScopeCloseMode::Cancel), ETaskScopeCloseResult::Closed);
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Scope.WaitFor(5.0), ETaskScopeWaitResult::Quiescent);
	ASSERT_EQ(Scheduler->Work.size(), 1u); Scheduler->Run(); EXPECT_EQ(Backend->Gets, 1u);
}

TEST(FAsyncCacheTests, AssetOperationCancellationWaitsForCallbackAndRejectsSelfWait)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("OperationCancel"), '8')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted, CallbackEntered, ReleaseCallback, CancelReturned;
	auto Producer = Tasks::LaunchTask("AssetOperationProducer", [&] {
		EXPECT_TRUE(Async->Get(Requests, *Operation, [&](auto, auto Result, auto) {
			EXPECT_FALSE(Result);
			EXPECT_EQ(Operation->Wait(), ERequestWaitResult::WouldBlock);
			CallbackEntered.Trigger(); ReleaseCallback.Wait();
		}));
		Tasks::AddNested(Operation->GetCompletionEvent()); Operation->CloseSubmission(); Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0));
	auto Cancel = Tasks::LaunchTask("AssetOperationCancel", [&] { Operation->Cancel(); CancelReturned.Trigger(); });
	EXPECT_TRUE(CallbackEntered.WaitFor(5.0));
	EXPECT_FALSE(CancelReturned.WaitFor(0.01));
	EXPECT_FALSE(Producer.IsCompleted()); EXPECT_FALSE(Operation->GetCompletionEvent().IsCompleted());
	ReleaseCallback.Trigger();
	EXPECT_EQ(Cancel.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_TRUE(Operation->GetCompletionEvent().IsCompleted()); EXPECT_FALSE(Busy.Poll());
	Scheduler->Run();
}

TEST(FAsyncCacheTests, AssetOperationUndispatchedRecordDoesNotRetainCoreShutdown)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	const auto Before = GetTaskSchedulerDiagnostics().NonterminalTaskCount;
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	ASSERT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, Before);
	ShutdownTaskSystem(ETaskShutdownMode::Cancel);
	Operation.reset();
	EXPECT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, 0u);
}

TEST(FAsyncCacheTests, AssetOperationDestructionCancelsQueuedRequestWithoutRetainingFacade)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	std::weak_ptr<FAssetCompilationOperation> Weak = Operation;
	const auto Completion = Operation->GetCompletionEvent().GetTaskHandle();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("OperationDestruction"), 'c')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	uint32 Calls = 0;
	auto Batch = Async->Get(Requests, *Operation, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; });
	ASSERT_TRUE(Batch);
	Operation.reset();
	EXPECT_TRUE(Weak.expired()); EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Completion.IsComplete());
	EXPECT_FALSE(Busy.Poll()); Scheduler->Run();
}

TEST(FAsyncCacheTests, AssetOperationSynchronousCompletionWaitsForSubmissionClosure)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), CreateInlineBuildScheduler(), {.Concurrency = 1});
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("OperationInline"), '9')}};
	uint32 Calls = 0;
	auto Producer = Tasks::LaunchTask("AssetOperationInline", [&] {
		for (uint32 Index = 0; Index != 2; ++Index)
		{
			EXPECT_TRUE(Async->Get(Requests, *Operation, [&](auto, auto, auto) { ++Calls; }));
			EXPECT_TRUE(Operation->Poll()); EXPECT_FALSE(Operation->GetCompletionEvent().IsCompleted());
		}
		Tasks::AddNested(Operation->GetCompletionEvent()); Operation->CloseSubmission();
	});
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls, 2u); EXPECT_TRUE(Operation->GetCompletionEvent().IsCompleted());
}

TEST(FAsyncCacheTests, AssetOperationWaitsForNativeHandlePublishedAfterSynchronousCallback)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FPublicationScheduler final : public IBuildScheduler
	{
	public:
		class FWork final : public IBuildScheduledWork
		{
		public:
			FTaskHandle Handle;
			auto GetTaskHandle() const -> FTaskHandle override { return Handle; }
			auto Wait() const -> ERequestWaitResult override
			{ return WaitTask(Handle).WaitStatus == ETaskWaitStatus::Completed ? ERequestWaitResult::Completed : ERequestWaitResult::WouldBlock; }
		};
		std::function<void()> BusyWork;
		Tasks::FTaskEvent* Completion = nullptr;
		FThreadEvent TailEntered, ReleaseTail;
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			if (!Completion) { BusyWork = std::move(Work); return std::shared_ptr<IBuildScheduledWork>{}; }
			Work();
			EXPECT_FALSE(Completion->IsCompleted()); // No native handle has been published yet.
			auto Scheduled = std::make_shared<FWork>();
			FTaskLaunchOptions Options; Options.bUseExplicitScope = true;
			auto Native = Durin::Private::TryLaunchCancelableTaskWithCompletion("OperationDispatchTail",
				[this, Work = std::move(Work)](const FTaskCancellationToken&) { TailEntered.Trigger(); ReleaseTail.Wait(); }, {}, Options);
			require(Native.HasValue()); Scheduled->Handle = std::move(Native).TakeValue(); return Scheduled;
		}
	};
	auto Scheduler = std::make_shared<FPublicationScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("OperationPublication"), 'a')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted;
	auto Producer = Tasks::LaunchTask("OperationLateDispatch", [&] {
		EXPECT_TRUE(Async->Get(Requests, *Operation, [](auto, auto, auto) {}));
		Tasks::AddNested(Operation->GetCompletionEvent()); Operation->CloseSubmission(); Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); Scheduler->Completion = &Operation->GetCompletionEvent();
	std::exchange(Scheduler->BusyWork, {})();
	EXPECT_TRUE(Scheduler->TailEntered.WaitFor(5.0));
	EXPECT_FALSE(Operation->GetCompletionEvent().IsCompleted()); EXPECT_FALSE(Producer.IsCompleted());
	Scheduler->ReleaseTail.Trigger();
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Operation->Wait(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, AssetOperationExceptionalProducerStillJoinsQueuedRetirement)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FAssetCompilationOperation>();
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("OperationException"), 'b')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted;
	auto Producer = Tasks::LaunchTask("OperationExceptionalProducer", [&] {
		try
		{
			EXPECT_TRUE(Async->Get(Requests, *Operation, [](auto, auto, auto) {}));
			throw std::runtime_error("after admission");
		}
		catch (...)
		{
			Tasks::AddNested(Operation->GetCompletionEvent()); Operation->CloseSubmission(); Submitted.Trigger(); throw;
		}
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_FALSE(Producer.IsCompleted());
	Operation->Cancel();
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Producer.GetState(), ETaskState::Failed);
	EXPECT_FALSE(Busy.Poll()); Scheduler->Run();
}

TEST(FAsyncCacheTests, MaximumInlineBatchesKeepDispatchDepthConstant)
{
	class FDepthScheduler final : public IBuildScheduler
	{
	public:
		uint32 Depth = 0, MaximumDepth = 0;
		auto IsInline() const -> bool override { return true; }
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			++Depth; MaximumDepth = std::max(MaximumDepth, Depth);
			Work(); --Depth; return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	auto Scheduler = std::make_shared<FDepthScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler); FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("InlineBatch"), 'a');
	std::vector<FCacheGetRequest> Gets(4096, FCacheGetRequest{.Key = Key});
	std::vector<uint32> Calls(Gets.size());
	auto Read = Async->Get(Gets, Owner, [&](uint32 Index, auto Result, auto) {
		EXPECT_TRUE(Result); ASSERT_LT(Index, Calls.size()); ++Calls[Index];
	});
	ASSERT_TRUE(Read); EXPECT_EQ((*Read)->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Scheduler->MaximumDepth, 1u); EXPECT_EQ(Backend->Gets, 4096u);
	for (auto Count : Calls) EXPECT_EQ(Count, 1u);
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2}))});
	std::vector<FCachePutRequest> Puts(4096, FCachePutRequest{Record});
	std::ranges::fill(Calls, 0u);
	auto Write = Async->Put(Puts, Owner, [&](uint32 Index, auto Result) {
		EXPECT_TRUE(Result); ASSERT_LT(Index, Calls.size()); ++Calls[Index];
	});
	ASSERT_TRUE(Write); EXPECT_EQ((*Write)->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Scheduler->MaximumDepth, 1u); EXPECT_EQ(Backend->Puts, 4096u);
	for (auto Count : Calls) EXPECT_EQ(Count, 1u);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, SameCacheInlineSubmissionCompletesInsideCallback)
{
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, CreateInlineBuildScheduler());
	FRequestOwner OuterOwner, InnerOwner;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("InlineNested"), 'b')}};
	uint32 Calls = 0;
	auto Outer = Async->Get(Requests, OuterOwner, [&](auto, auto Result, auto) {
		EXPECT_TRUE(Result); ++Calls;
		auto Inner = Async->Get(Requests, InnerOwner, [&](auto, auto NestedResult, auto) {
			EXPECT_TRUE(NestedResult); ++Calls;
			EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::WouldBlock);
		});
		ASSERT_TRUE(Inner); EXPECT_EQ((*Inner)->Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(InnerOwner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Calls, 2u);
	});
	ASSERT_TRUE(Outer); EXPECT_EQ((*Outer)->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 2u); EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, RetainedPayloadCapacityRejectsAtomicallyAndReleasesOnCancellation)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .MaximumRetainedBytes = 14000});
	FRequestOwner Busy, Pending, Rejected;
	const auto Key = MakeKey(FCacheBucket::FromString("RetainedCapacity"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{3}))});
	const std::array Writes{FCachePutRequest{Record}};
	const std::array Batch{FCachePutRequest{Record}, FCachePutRequest{Record}};
	uint32 Calls = 0;
	auto TooLarge = Async->Put(Batch, Rejected, [&](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(TooLarge); EXPECT_EQ(TooLarge.error().Code, ECacheError::Capacity);
	EXPECT_TRUE(Rejected.Poll()); EXPECT_TRUE(Scheduler->Work.empty());
	const std::array Reads{FCacheGetRequest{.Key = Key}};
	ASSERT_TRUE(Async->Get(Reads, Busy, [](auto, auto, auto) {}));
	ASSERT_TRUE(Async->Put(Writes, Pending, [&](auto, auto Result) {
		ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Calls;
	}));
	EXPECT_EQ(Scheduler->Work.size(), 1u);
	auto Full = Async->Put(Writes, Rejected, [&](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Full); EXPECT_EQ(Full.error().Code, ECacheError::Capacity); EXPECT_TRUE(Rejected.Poll());
	Pending.Cancel(); EXPECT_EQ(Calls, 1u);
	ASSERT_TRUE(Async->Put(Writes, Rejected, [&](auto, auto Result) { EXPECT_TRUE(Result); ++Calls; }));
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Calls, 2u); EXPECT_EQ(Backend->Puts, 1u);
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
	// Admission charges the retained allocation, not merely the one-byte view.
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(65536, std::byte{4}));
	const std::array Sliced{FCachePutRequest{RecordWith(Key, {Backing.MakeView(0, 1)})}};
	auto Slice = Async->Put(Sliced, Rejected, [&](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Slice); EXPECT_EQ(Slice.error().Code, ECacheError::Capacity); EXPECT_TRUE(Rejected.Poll());
}

TEST(FAsyncCacheTests, RetainedBytesRemainChargedDuringCallbackAndRecoverAfterward)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .MaximumRetainedBytes = 14000});
	FRequestOwner Owner, Other;
	const auto Key = MakeKey(FCacheBucket::FromString("RetainedCallback"), 'd');
	const std::array Requests{FCachePutRequest{RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{5}))})}};
	ASSERT_TRUE(Async->Put(Requests, Owner, [&](auto, auto Result) {
		EXPECT_TRUE(Result);
		auto Nested = Async->Put(Requests, Other, [&](auto, auto) { ADD_FAILURE(); });
		ASSERT_FALSE(Nested); EXPECT_EQ(Nested.error().Code, ECacheError::Capacity); EXPECT_TRUE(Other.Poll());
	}));
	Scheduler->Run(); EXPECT_TRUE(Owner.Poll());
	ASSERT_TRUE(Async->Put(Requests, Other, [](auto, auto Result) { EXPECT_TRUE(Result); }));
	Scheduler->Run(); EXPECT_EQ(Backend->Puts, 2u); EXPECT_TRUE(Other.Poll());
}

TEST(FAsyncCacheTests, OldestRequestPreventsStarvationUnderHighPriorityArrivals)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 8});
	FRequestOwner High(ERequestPriority::High), Low(ERequestPriority::Low);
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("FairQueue"), 'e')}};
	uint32 LowCalls = 0;
	ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {}));
	ASSERT_TRUE(Async->Get(Requests, Low, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++LowCalls; }));
	for (uint32 Turn = 0; Turn < 33; ++Turn) {
		ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {})); Scheduler->Run();
	}
	ASSERT_EQ(Scheduler->Priorities.size(), 34u);
	EXPECT_EQ(Scheduler->Priorities[33], ERequestPriority::Low);
	Scheduler->Run(); EXPECT_EQ(LowCalls, 1u);
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(High.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Low.Wait(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, RetainedAdmissionDeduplicatesRecordBackingAndChargesCopiedSelections)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .MaximumRetainedBytes = 14000});
	FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("RetainedTables"), 'a');
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{6}));
	const std::array Writes{FCachePutRequest{RecordWith(Key, {Backing, Backing})}};
	ASSERT_TRUE(Async->Put(Writes, Owner, [](auto, auto Result) { EXPECT_TRUE(Result); }));
	Scheduler->Run(); EXPECT_EQ(Backend->Puts, 1u);
	std::vector<FValueId> Ids; Ids.reserve(4096);
	for (uint32 Index = 0; Index < 4096; ++Index) Ids.push_back(FValueId::FromName("Selection").MakeIndexed(Index));
	const std::array Reads{FCacheGetRequest{.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Ids}};
	auto Rejected = Async->Get(Reads, Owner, [](auto, auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Rejected); EXPECT_EQ(Rejected.error().Code, ECacheError::Capacity);
	EXPECT_TRUE(Owner.Poll()); EXPECT_TRUE(Scheduler->Work.empty()); EXPECT_EQ(Backend->Gets, 0u);
}

TEST(FAsyncCacheTests, MaximumCapacityQueueKeepsOldestProgressUnderPriorityArrivals)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner High(ERequestPriority::High), Low(ERequestPriority::Low);
	const auto Key = MakeKey(FCacheBucket::FromString("FullFairQueue"), 'a');
	const std::array Requests{FCacheGetRequest{.Key = Key}};
	std::vector<FCacheGetRequest> Waiting(4094, Requests[0]);
	uint32 Calls = 0;
	ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {}));
	ASSERT_TRUE(Async->Get(Waiting, Low, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Calls; }));
	for (uint32 Turn = 0; Turn < 33; ++Turn)
	{
		ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {}));
		if (Turn == 0)
		{
			auto Full = Async->Get(Requests, High, [](auto, auto, auto) { ADD_FAILURE(); });
			ASSERT_FALSE(Full); EXPECT_EQ(Full.error().Code, ECacheError::Capacity);
		}
		Scheduler->Run();
	}
	ASSERT_EQ(Scheduler->Priorities.size(), 34u);
	EXPECT_EQ(Scheduler->Priorities[33], ERequestPriority::Low);
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Calls, Waiting.size()); EXPECT_TRUE(Low.Poll()); EXPECT_TRUE(High.Poll());
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, QueuedDataBudgetCoversMetadataAndRecoversAfterDispatchRejection)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	const auto Metadata = MakeBuildMetadata(FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{1}))).value();
	ASSERT_TRUE(Builder.AddMeta(FValueId::FromName("Metadata"), Metadata));
	ASSERT_TRUE(Builder.AddMessage(EBuildMessageSeverity::Warning, std::string(4096, 'x')));
	const auto Record = FCacheRecord::FromOutput(MakeKey(FCacheBucket::FromString("QueuedData"), 'b'), std::move(Builder).Build().value()).value();
	const uint64 Bytes = Record.GetSchema().size() + Metadata.GetBytes().GetRetainedCapacityBytes() + 4096;
	const std::array Writes{FCachePutRequest{Record}};
	FRequestOwner Owner;
	auto Small = CreateAsyncCache(Backend, Scheduler, {.MaximumRetainedBytes = Bytes - 1});
	auto Rejected = Small->Put(Writes, Owner, [](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Rejected); EXPECT_EQ(Rejected.error().Code, ECacheError::Capacity); EXPECT_TRUE(Owner.Poll());
	auto Exact = CreateAsyncCache(Backend, Scheduler, {.MaximumRetainedBytes = Bytes});
	Scheduler->Reject = true;
	ASSERT_TRUE(Exact->Put(Writes, Owner, [](auto, auto Result) {
		ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}));
	EXPECT_TRUE(Owner.Poll());
	Scheduler->Reject = false;
	ASSERT_TRUE(Exact->Put(Writes, Owner, [](auto, auto Result) { EXPECT_TRUE(Result); }));
	Scheduler->Run(); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Backend->Puts, 1u);
}

namespace
{
	class FResourceSessionFunction final : public IBuildFunction
	{
	public:
		auto GetName() const -> std::string_view override { return "Resource.SessionFixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& Context) const -> void override
		{ Context.SetConstantsSchema(1); Context.SetOutput("Fixture.Output", 1); Context.SetCacheBucket(FCacheBucket::FromString("ResourceSession")); }
		auto Build(FBuildContext& Context) const -> void override
		{ Context.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(Bytes({1}))); }
	};
	auto ResourceDefinition() -> FBuildDefinition
	{
		static std::once_flag Once;
		std::call_once(Once, [] { require(GetBuild().Register(std::make_shared<FResourceSessionFunction>())); });
		return std::move(FBuildDefinitionBuilder("Resource.SessionFixture")).Build().value();
	}
	auto ResourceOptions() -> FBuildRequestOptions
	{ return {.Policy = {.QueryCache = false, .StoreOnBuild = false}}; }
}

TEST(FResourceBuildSessionTests, InlineNestedBuildAndTypedAdmissionSurviveCloseAndReopen)
{
	AssetPrivate::FResourceBuildSessions Sessions;
	const auto Definition = ResourceDefinition();
	auto Session = Sessions.Get(EBuildCacheExecution::Inline); ASSERT_TRUE(Session);
	FRequestOwner Owner; uint32 Calls = 0;
	ASSERT_TRUE((*Session)->Build(Definition, Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls;
		auto Nested = Sessions.Build(Definition, {}, ResourceOptions());
		ASSERT_TRUE(Nested); EXPECT_EQ(Nested->GetStatus(), EStatus::Ok); ++Calls;
	}, {}, ResourceOptions()));
	EXPECT_EQ(Calls, 2u); EXPECT_TRUE(Owner.Poll());
	auto Unknown = Sessions.Build(std::move(FBuildDefinitionBuilder("Resource.Unknown")).Build().value(), {}, ResourceOptions());
	ASSERT_FALSE(Unknown); EXPECT_EQ(Unknown.error().Reason, EBuildAdmissionReason::MissingFunction);
	EXPECT_EQ(Sessions.Close(), EBuildDrainResult::Drained);
	auto Closed = Sessions.Build(Definition, {}, ResourceOptions());
	ASSERT_FALSE(Closed); EXPECT_EQ(Closed.error().Reason, EBuildAdmissionReason::Closed);
	Sessions.Open(); ASSERT_TRUE(Sessions.Build(Definition, {}, ResourceOptions()));
	EXPECT_EQ(Sessions.Close(), EBuildDrainResult::Drained);
}

TEST(FResourceBuildSessionTests, ReentrantCloseRetainsSessionsUntilExternalDrain)
{
	AssetPrivate::FResourceBuildSessions Sessions;
	const auto Definition = ResourceDefinition();
	auto Session = Sessions.Get(EBuildCacheExecution::Inline); ASSERT_TRUE(Session);
	FRequestOwner Owner;
	ASSERT_TRUE((*Session)->Build(Definition, Owner, [&](auto) {
		EXPECT_EQ(Sessions.Close(), EBuildDrainResult::WouldBlock);
		auto Closed = Sessions.Get(EBuildCacheExecution::Inline);
		ASSERT_FALSE(Closed); EXPECT_EQ(Closed.error().Reason, EBuildAdmissionReason::Closed);
	}, {}, ResourceOptions()));
	EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Sessions.Close(), EBuildDrainResult::Drained);
}

TEST(FResourceBuildSessionTests, ReplacingScopedTaskServiceDrainsOutsideAcquisitionLock)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	AssetPrivate::FResourceBuildSessions Sessions;
	const auto Definition = ResourceDefinition();
	const auto MakeService = [] {
		auto Service = CreateBuild(); require(Service->Register(std::make_shared<FResourceSessionFunction>())); return Service;
	};
	ASSERT_EQ(Sessions.SetTaskService(MakeService()), EBuildDrainResult::Drained);
	auto Old = Sessions.Get(EBuildCacheExecution::Task); ASSERT_TRUE(Old);
	FRequestOwner Owner;
	FThreadEvent Entered, Release; std::atomic<bool> Reenter = true;
	struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } ReleaseOnExit{Release};
	ASSERT_TRUE((*Old)->Build(Definition, Owner, [&](auto) {
		Entered.Trigger(); Release.Wait();
		if (Reenter) EXPECT_TRUE(Sessions.Get(EBuildCacheExecution::Task));
	}, {}, ResourceOptions()));
	ASSERT_TRUE(Entered.WaitFor(5.0));
	auto Replacement = std::async(std::launch::async, [&] { return Sessions.SetTaskService(MakeService()); });
	auto Acquisition = std::async(std::launch::async, [&] {
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < Deadline) {
			auto Current = Sessions.Get(EBuildCacheExecution::Task);
			if (!Current || *Current != *Old) return Current.has_value();
			std::this_thread::yield();
		}
		return false;
	});
	const auto Status = Acquisition.wait_for(std::chrono::seconds(5));
	Reenter = Status == std::future_status::ready;
	EXPECT_EQ(Status, std::future_status::ready);
	EXPECT_EQ(Replacement.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
	Release.Trigger();
	EXPECT_TRUE(Acquisition.get()); EXPECT_EQ(Replacement.get(), EBuildDrainResult::Drained);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Sessions.SetTaskService({}), EBuildDrainResult::Drained);
	EXPECT_EQ(Sessions.Close(), EBuildDrainResult::Drained);
}

TEST(FAsyncCacheTests, ItemAndByteCapacityAreDistinctFromBackendFailure)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 1});
	FRequestOwner Owner, Other;
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("CapacityCode"), 'f')}};
	Backend->Throw = true;
	ASSERT_TRUE(Async->Get(Requests, Owner, [](auto, auto Result, auto) {
		ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}));
	auto Overflow = Async->Get(Requests, Other, [](auto, auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Overflow); EXPECT_EQ(Overflow.error().Code, ECacheError::Capacity); EXPECT_TRUE(Other.Poll());
	Scheduler->Run();
	auto ZeroBytes = CreateAsyncCache(Backend, Scheduler, {.MaximumRetainedBytes = 0});
	// A selection-free Get retains no queued data; fixed overhead uses item admission.
	ASSERT_TRUE(ZeroBytes->Get(Requests, Other, [](auto, auto, auto) {}));
	Scheduler->Run(); EXPECT_TRUE(Other.Poll());
	const std::array Writes{FCachePutRequest{RecordWith(Requests[0].Key, {FSharedByteBuffer::Take(Bytes({1}))})}};
	auto Rejected = ZeroBytes->Put(Writes, Other, [](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Rejected); EXPECT_EQ(Rejected.error().Code, ECacheError::Capacity); EXPECT_TRUE(Other.Poll());
}

TEST(FAsyncCacheTests, OwnsBatchSelectionsAndBoundsConcurrencyAndAdmission)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 3}); FRequestOwner Owner(ERequestPriority::High);
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'a');
	std::vector<FValueId> Ids{FValueId::FromName("Selected")}; const auto Original = Ids;
	std::array Requests{FCacheGetRequest{.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Ids}, FCacheGetRequest{.Key = Key}};
	uint32 Callbacks = 0; uint32 Indices = 0; FCacheQueryStats Borrowed; Requests[0].Stats = &Borrowed;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](uint32 Index, auto Result, auto Stats) { EXPECT_TRUE(Result); EXPECT_EQ(Stats.ReadBytes, 17u); ++Callbacks; Indices |= 1 << Index; }));
	Ids.clear(); Requests = {}; EXPECT_FALSE(Owner.Poll()); EXPECT_EQ(Scheduler->Work.size(), 1u);
	std::array Extra{FCacheGetRequest{Key}, FCacheGetRequest{Key}};
	EXPECT_FALSE(Async->Get(Extra, Owner, [&](auto, auto, auto) { ADD_FAILURE(); })); // Entire batch is rejected.
	Scheduler->Run(); EXPECT_EQ(Backend->ObservedIds, Original); EXPECT_EQ(Scheduler->Work.size(), 1u);
	Scheduler->Run(); EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Indices, 3u); EXPECT_EQ(Borrowed.ReadBytes, 0u);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
	for (auto Priority : Scheduler->Priorities) EXPECT_EQ(Priority, ERequestPriority::High);
}

TEST(FAsyncCacheTests, PrioritizesQueuedOwnersAndDetectsNestedCallbackWaits)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 4}); FRequestOwner Low(ERequestPriority::Low), High(ERequestPriority::High);
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'e'); const std::array Requests{FCacheGetRequest{Key}, FCacheGetRequest{Key}};
	ASSERT_TRUE(Async->Get(Requests, Low, [](auto, auto, auto) {})); ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {}));
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Scheduler->Priorities, (std::vector{ERequestPriority::Low, ERequestPriority::High, ERequestPriority::High, ERequestPriority::Low}));
	auto Outer = CreateAsyncCache(Backend, CreateInlineBuildScheduler()); auto Inner = CreateAsyncCache(Backend, CreateInlineBuildScheduler());
	FRequestOwner OuterOwner, InnerOwner; uint32 Callbacks = 0; const std::array One{FCacheGetRequest{Key}};
	ASSERT_TRUE(Outer->Get(One, OuterOwner, [&](auto, auto, auto) {
		ASSERT_TRUE(Inner->Get(One, InnerOwner, [&](auto, auto, auto) { EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::WouldBlock); EXPECT_EQ(Outer->Drain(), ERequestWaitResult::WouldBlock); ++Callbacks; }));
		++Callbacks;
	}));
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(InnerOwner.Wait(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, CancellationDropsExceptionsAndCallbackRetirementCompleteExactlyOnce)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1}); const std::array Requests{FCacheGetRequest{MakeKey(FCacheBucket::FromString("Async"), 'b')}};
	uint32 Callbacks = 0;
	{
		FRequestOwner Owner;
		ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Callbacks; }));
		Owner.Cancel(); Scheduler->Run(); EXPECT_EQ(Backend->Gets, 0u); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	}
	{
		FRequestOwner Owner;
		ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Callbacks; }));
		Scheduler->Work.clear(); EXPECT_TRUE(Owner.Poll());
	}
	{
		Backend->Throw = true; auto Owner = std::make_unique<FRequestOwner>();
		ASSERT_TRUE(Async->Get(Requests, *Owner, [&](auto, auto Result, auto) {
			ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
			EXPECT_EQ(Owner->Wait(), ERequestWaitResult::WouldBlock); EXPECT_EQ(Async->Drain(), ERequestWaitResult::WouldBlock);
			Owner->RequestCancel(); ++Callbacks; throw std::runtime_error("callback");
		}));
		Scheduler->Run(); Owner.reset(); EXPECT_FALSE(Owner);
	}
	EXPECT_EQ(Callbacks, 3u); EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
	FRequestOwner Owner; Scheduler->Reject = true;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure); ++Callbacks; }));
	EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Callbacks, 4u);
	Async->Close(); EXPECT_FALSE(Async->Get(Requests, Owner, [&](auto, auto, auto) { ADD_FAILURE(); }));
}

TEST(FAsyncCacheTests, CloseDrainsAcceptedBatchAndRetainsPutPayloads)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1}); FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'c');
	std::array Requests{FCachePutRequest{RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2}))})}};
	uint32 Callbacks = 0;
	ASSERT_TRUE(Async->Put(Requests, Owner, [&](auto, auto Result) { EXPECT_TRUE(Result); ++Callbacks; }));
	Requests = {}; Scheduler->Run(); EXPECT_EQ(Backend->Puts, 1u);
	const std::array Reads{FCacheGetRequest{Key}, FCacheGetRequest{Key}};
	ASSERT_TRUE(Async->Get(Reads, Owner, [&](auto, auto Result, auto) { ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Callbacks; }));
	Async->Close(); while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Callbacks, 3u); EXPECT_EQ(Backend->Gets, 0u); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, OwnerPriorityChangeReordersQueuedCacheRequests)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner First, Promoted(ERequestPriority::Low), Other;
	const std::array Reads{FCacheGetRequest{MakeKey(FCacheBucket::FromString("Async"), 'e')}};
	std::vector<uint32> Order;
	ASSERT_TRUE(Async->Get(Reads, First, [&](auto, auto, auto) { Order.push_back(0); }));
	ASSERT_TRUE(Async->Get(Reads, Promoted, [&](auto, auto, auto) { Order.push_back(1); }));
	ASSERT_TRUE(Async->Get(Reads, Other, [&](auto, auto, auto) { Order.push_back(2); }));
	Promoted.SetPriority(ERequestPriority::High);
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Order, (std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(Scheduler->Priorities, (std::vector{ERequestPriority::Normal, ERequestPriority::High, ERequestPriority::Normal}));
	EXPECT_TRUE(Promoted.Poll());
}

TEST(FAsyncCacheTests, SharedQueueNeverInheritsEitherSubmittingOrPumpingScope)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FScopeCache final : public ICache
	{
	public:
		mutable std::atomic<uint32> Gets = 0;
		std::promise<void> Entered;
		std::shared_future<void> Release;
		auto Get(const FCacheGetRequest&) const -> FCacheGetResult override
		{
			if (++Gets == 1) { const_cast<FScopeCache*>(this)->Entered.set_value(); Release.wait(); }
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutRequest&) const -> FCachePutResult override { return {}; }
	};
	auto Backend = std::make_shared<FScopeCache>();
	std::promise<void> Release;
	Backend->Release = Release.get_future().share();
	auto Entered = Backend->Entered.get_future();
	auto Async = CreateAsyncCache(Backend, {}, {.Concurrency = 1});
	Tasks::FTaskGroup FirstScope, SecondScope;
	auto FirstOperation = std::make_shared<FAssetCompilationOperation>(), SecondOperation = std::make_shared<FAssetCompilationOperation>();
	auto& First = *FirstOperation; auto& Second = *SecondOperation;
	const std::array Reads{FCacheGetRequest{MakeKey(FCacheBucket::FromString("Async"), 'f')}};
	std::atomic<uint32> Calls = 0;
	FThreadEvent FirstSubmitted, SecondSubmitted;
	auto FirstSubmit = Tasks::LaunchTask(FirstScope, Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Reads, First, [&](auto, auto Result, auto) {
			EXPECT_TRUE(Result);
			EXPECT_EQ(Durin::Private::FTaskRuntimeAccess::GetCurrentScope(), FTaskScopeToken{});
			++Calls;
		}));
		Tasks::AddNested(First.GetCompletionEvent()); First.CloseSubmission(); FirstSubmitted.Trigger();
	});
	EXPECT_TRUE(FirstSubmitted.WaitFor(5.0));
	const auto Started = Entered.wait_for(std::chrono::seconds(5));
	auto SecondSubmit = Tasks::LaunchTask(SecondScope, Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Reads, Second, [&](auto, auto Result, auto) {
			EXPECT_TRUE(Result);
			EXPECT_EQ(Durin::Private::FTaskRuntimeAccess::GetCurrentScope(), FTaskScopeToken{});
			++Calls;
		}));
		Tasks::AddNested(Second.GetCompletionEvent()); Second.CloseSubmission(); SecondSubmitted.Trigger();
	});
	EXPECT_TRUE(SecondSubmitted.WaitFor(5.0));
	EXPECT_FALSE(FirstSubmit.IsCompleted()); EXPECT_FALSE(SecondSubmit.IsCompleted());
	Release.set_value();
	EXPECT_EQ(Started, std::future_status::ready);
	EXPECT_EQ(First.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Second.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(FirstSubmit.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(SecondSubmit.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls.load(), 2u);
	EXPECT_EQ(FirstScope.Close(), ETaskScopeCloseResult::Closed);
	EXPECT_EQ(SecondScope.Close(), ETaskScopeCloseResult::Closed);
}

TEST(FAsyncCacheTests, CoreShutdownDrainsRunningAndPendingAcceptedRequests)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FBlockingCache final : public ICache
	{
	public:
		std::promise<void> Entered;
		std::shared_future<void> Release;
		mutable std::atomic<uint32> Gets = 0;
		auto Get(const FCacheGetRequest&) const -> FCacheGetResult override
		{ if (++Gets == 1) const_cast<FBlockingCache*>(this)->Entered.set_value(); Release.wait(); return std::optional<FCacheRecord>{}; }
		auto Put(const FCachePutRequest&) const -> FCachePutResult override { return {}; }
	};
	std::promise<void> Release; auto Cache = std::make_shared<FBlockingCache>(); Cache->Release = Release.get_future().share();
	auto Entered = Cache->Entered.get_future(); auto Async = CreateAsyncCache(Cache, {}, {.Concurrency = 1}); FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'd'); const std::array Requests{FCacheGetRequest{Key}, FCacheGetRequest{Key}};
	std::atomic<uint32> Callbacks = 0;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Callbacks; }));
	const auto Started = Entered.wait_for(std::chrono::seconds(5));
	Async->Close(); Release.set_value(); ShutdownTaskScheduler(false);
	EXPECT_EQ(Started, std::future_status::ready); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Async->Drain(), ERequestWaitResult::Completed); EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Cache->Gets, 1u);
}

TEST(FAsyncCacheTests, CoalescesOnlyInflightBlobReadsAndReleasesFailures)
{
	FCacheReadCoalescer Coalescer; std::latch Entered(1), Release(1);
	uint64 FirstBytes = 0, SecondBytes = 0; std::atomic<uint32> Reads = 0;
	auto Read = [&]() -> FCacheStorageGetResult { if (++Reads == 1) Entered.count_down(); Release.wait(); return std::optional{FSharedByteBuffer::Take(Bytes({1, 2, 3}))}; };
	auto First = std::async(std::launch::async, [&] { return Coalescer.Get("blob", Read, &FirstBytes); }); Entered.wait();
	auto Second = std::async(std::launch::async, [&] { return Coalescer.Get("blob", Read, &SecondBytes); });
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Coalescer.GetCoalescedReadCount() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	const bool Joined = Coalescer.GetCoalescedReadCount() == 1; Release.count_down();
	auto A = First.get(); auto B = Second.get(); ASSERT_TRUE(Joined); ASSERT_TRUE(A && *A && B && *B);
	EXPECT_EQ(Reads, 1u); EXPECT_EQ(FirstBytes, 3u); EXPECT_EQ(SecondBytes, 0u); EXPECT_TRUE((**A).SharesStorageWith(**B));
	auto Failure = Coalescer.Get("blob", []() -> FCacheStorageGetResult { throw std::runtime_error("read"); }); EXPECT_FALSE(Failure);
	auto Retry = Coalescer.Get("blob", []() -> FCacheStorageGetResult { return std::optional{FSharedByteBuffer::Take(Bytes({4}))}; }); ASSERT_TRUE(Retry && *Retry);
	EXPECT_EQ((**Retry).GetSize(), 1u);
}

TEST(FAsyncCacheTests, CanceledPersistenceNeverPublishesReplacementManifest)
{
	FScopedCacheDirectory Directory("canceled-persistence"); const auto Key = MakeKey(FCacheBucket::FromString("Cancel"), 'a');
	const auto Original = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1}))}); ASSERT_TRUE(GetCache().Put({Original}));
	const auto Replacement = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({2})), FSharedByteBuffer::Take(Bytes({3}))});
	for (uint32 StopAt : {1u, 2u, 3u, 4u, 5u, 6u})
	{
		uint32 Checks = 0;
		auto Saved = GetCache().Put({.Record = Replacement, .ShouldCancel = [&] { return ++Checks == StopAt; }});
		ASSERT_FALSE(Saved); EXPECT_EQ(Saved.error().Code, ECacheError::Canceled);
		auto Found = GetCache().Get({Key}); ASSERT_TRUE(Found && *Found); EXPECT_EQ((**Found).GetValues().size(), 1u);
		EXPECT_EQ((**Found).GetValues()[0].Value.GetRawHash(), Original.GetValues()[0].Value.GetRawHash());
	}
}

TEST(FCacheSelectionTests, MetadataAndSelectionLeaveOtherValuesAsReferences)
{
	FScopedCacheDirectory Directory("cache-selection");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{1})),
		FSharedByteBuffer::Take(Bytes({2, 3})), FSharedByteBuffer{}});
	ASSERT_TRUE(GetCache().Put({Record}));
	FCacheQueryStats Stats;
	auto Metadata = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::MetadataOnly, .Stats = &Stats});
	ASSERT_TRUE(Metadata && *Metadata); EXPECT_EQ(Stats.ValuesRead, 0u); EXPECT_EQ(Stats.DecodedBytes, 0u);
	EXPECT_EQ(Stats.ReadBytes, Record.EncodeManifest()->GetSize());
	EXPECT_EQ(GetBuildMetadataPayload(**Metadata).GetSize(), 2u);
	EXPECT_FALSE((**Metadata).ToOutput(Key));
	for (const auto& Item : (**Metadata).GetValues()) EXPECT_FALSE(Item.Value.HasData());
	const std::array Selected{FValueId::FromName("Block").MakeIndexed(2), FValueId::FromName("Block").MakeIndexed(1)};
	auto Partial = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected, .Stats = &Stats});
	ASSERT_TRUE(Partial && *Partial); EXPECT_EQ(Stats.ValuesRead, 2u); EXPECT_EQ(Stats.DecodedBytes, 2u);
	EXPECT_FALSE((**Partial).FindValue(FValueId::FromName("Block").MakeIndexed(0))->HasData());
	EXPECT_TRUE((**Partial).FindValue(Selected[0])->HasData()); // Empty data remains resident.
	EXPECT_TRUE((**Partial).FindValue(Selected[1])->HasData());
	EXPECT_FALSE((**Partial).ToOutput(Key));
	auto Full = GetCache().Get({Key}); ASSERT_TRUE(Full && *Full); EXPECT_TRUE((**Full).ToOutput(Key));
	std::vector<FValueId> All;
	for (const auto& Value : Record.GetValues()) All.push_back(Value.Id);
	auto AllSelected = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = All});
	ASSERT_TRUE(AllSelected && *AllSelected); EXPECT_TRUE((**AllSelected).ToOutput(Key));
	auto EmptySelected = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .Stats = &Stats});
	ASSERT_TRUE(EmptySelected && *EmptySelected); EXPECT_EQ(Stats.ValuesRead, 0u);
}

TEST(FCacheSelectionTests, RejectsInvalidSelectionAndDistinguishesMissFromMissingValue)
{
	FScopedCacheDirectory Directory("cache-selection-errors");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'b');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	const std::array Selected{Record.GetValues()[0].Id};
	auto Miss = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected});
	ASSERT_TRUE(Miss); EXPECT_FALSE(*Miss);
	ASSERT_TRUE(GetCache().Put({Record}));
	const std::array Duplicate{Selected[0], Selected[0]};
	const std::array Null{FValueId{}};
	for (const auto& Request : {FCacheGetRequest{.Key = Key, .RequiredValues = Selected},
		FCacheGetRequest{.Key = Key, .Mode = ECacheGetMode::MetadataOnly, .RequiredValues = Selected},
		FCacheGetRequest{.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Duplicate},
		FCacheGetRequest{.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Null}})
	{
		auto Result = GetCache().Get(Request); ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::InvalidRequest);
	}
	const std::array Unknown{FValueId::FromName("Unknown")}; FCacheQueryStats Stats;
	auto Missing = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Unknown, .Stats = &Stats});
	ASSERT_FALSE(Missing); EXPECT_EQ(Missing.error().Code, ECacheError::MissingValue); EXPECT_EQ(Stats.ValuesRead, 0u);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({9}), EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1")));
	auto Undamaged = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected});
	ASSERT_TRUE(Undamaged && *Undamaged); EXPECT_FALSE(GetCache().Get({Key}));
	std::filesystem::remove(EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1"));
	auto Partial = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected});
	ASSERT_TRUE(Partial && *Partial); EXPECT_FALSE(GetCache().Get({Key}));
	std::filesystem::remove(EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1"));
	Missing = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected});
	ASSERT_FALSE(Missing); EXPECT_EQ(Missing.error().Code, ECacheError::MissingValue);
	auto Metadata = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::MetadataOnly}); ASSERT_TRUE(Metadata && *Metadata);
}

TEST(FCacheSelectionTests, EnforcesSelectedBudgetsAndCooperativeCancellation)
{
	FScopedCacheDirectory Directory("cache-selection-budget");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(1024 * 1024, std::byte{1})),
		FSharedByteBuffer::Take(Bytes({2}))}); ASSERT_TRUE(GetCache().Put({Record}));
	const std::array Selected{FValueId::FromName("Block").MakeIndexed(1)};
	// Logical output bounds still validate every descriptor, not only resident values.
	EXPECT_FALSE(GetCache().Get({.Key = Key, .OutputLimits = {.MaximumTotalBytes = 1024},
		.Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected}));
	FCacheQueryStats Stats;
	auto Partial = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected, .Stats = &Stats});
	ASSERT_TRUE(Partial && *Partial);
	EXPECT_TRUE(GetCache().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes,
		.MaximumWorkingSetBytes = Stats.PeakWorkingSetBytes, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected}));
	EXPECT_FALSE(GetCache().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes - 1,
		.Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected}));
	EXPECT_FALSE(GetCache().Get({.Key = Key, .MaximumWorkingSetBytes = Stats.PeakWorkingSetBytes}));
	for (uint32 StopAt : {1u, 2u, 3u, 4u, 5u, 6u})
	{
		uint32 Checks = 0;
		auto Canceled = GetCache().Get({.Key = Key, .Mode = ECacheGetMode::SelectedValues, .RequiredValues = Selected,
			.ShouldCancel = [&] { return ++Checks == StopAt; }, .Stats = &Stats});
		ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
		if (StopAt == 1) EXPECT_EQ(Stats.ReadBytes, 0u);
		if (StopAt <= 4) EXPECT_EQ(Stats.ValuesRead, 0u);
		if (StopAt == 5) EXPECT_EQ(Stats.DecodedBytes, 0u);
		if (StopAt == 6) EXPECT_EQ(Stats.DecodedBytes, 1u);
	}
}

TEST(FCacheValueTests, CompressesLargeValuesAndStoresSmallOrIncompressibleValuesRaw)
{
	FByteBuffer Random(8192); uint32 State = 123;
	for (auto& Byte : Random) { State ^= State << 13; State ^= State >> 17; State ^= State << 5; Byte = std::byte(State & 255); }
	const FValue Small(FSharedByteBuffer::Take(Bytes({1, 2, 3})));
	const FValue Noise(FSharedByteBuffer::Take(std::move(Random)));
	const FValue Large(FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{9})));
	for (const auto& Value : {FValue(FSharedByteBuffer{}), Small, Noise, Large})
	{
		auto Blob = FCompressedCacheValue::Compress(Value, 16384, 16 * 1024 * 1024); ASSERT_TRUE(Blob);
		EXPECT_EQ(Blob->IsCompressed(), Value.GetRawHash() == Large.GetRawHash());
		auto Loaded = FCompressedCacheValue::Load(Blob->GetEncoded(), 8192, 16384); ASSERT_TRUE(Loaded);
		auto Raw = Loaded->Decompress(1024 * 1024); ASSERT_TRUE(Raw);
		EXPECT_TRUE(Raw->HasData()); EXPECT_EQ(Raw->GetRawHash(), Value.GetRawHash());
		EXPECT_TRUE(std::ranges::equal(Raw->GetData().GetBytes(), Value.GetData().GetBytes()));
		if (!Loaded->IsCompressed() && Value.GetRawSize()) EXPECT_TRUE(Raw->GetData().SharesStorageWith(Blob->GetEncoded()));
	}
}

TEST(FCacheValueTests, BoundsWorkspaceAndValidatesRawIdentityAfterEnvelopeValidation)
{
	const FValue Value(FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{3})));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value.RemoveData(), 8192, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value, 63, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value, 8192, 63));
	auto Raw = FCompressedCacheValue::Compress(Value, 8192, 8192); ASSERT_TRUE(Raw);
	EXPECT_FALSE(Raw->IsCompressed()); // Insufficient compression workspace uses bounded raw fallback.
	EXPECT_FALSE(FCompressedCacheValue::Load(Raw->GetEncoded(), 4095, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Load(Raw->GetEncoded(), 4096, 64));
	FByteBuffer Body(Raw->GetEncoded().begin(), Raw->GetEncoded().end() - 16); Body[48] ^= std::byte{1};
	FBinaryWriter Writer; Writer.WriteBytes(Body); Writer.WriteHash128(FXxHash128::HashBuffer(Body));
	auto Forged = FCompressedCacheValue::Load(FSharedByteBuffer::Take(Writer.TakeBytes()), 4096, 8192); ASSERT_TRUE(Forged);
	EXPECT_FALSE(Forged->Decompress(8192));
	auto Compressed = FCompressedCacheValue::Compress(Value, 8192, 16 * 1024 * 1024); ASSERT_TRUE(Compressed);
	ASSERT_TRUE(Compressed->IsCompressed()); EXPECT_FALSE(Compressed->Decompress(4095));
	FByteBuffer Damaged(Compressed->GetEncoded().begin(), Compressed->GetEncoded().end()); Damaged.back() ^= std::byte{1};
	EXPECT_FALSE(FCompressedCacheValue::Load(FSharedByteBuffer::Take(std::move(Damaged)), 4096, 8192));
}

TEST(FCacheTests, ReusesContentAcrossBucketsAndRoundTripsCompleteOutputs)
{
	FScopedCacheDirectory Directory("ContentReuse");
	const auto A = MakeKey(FCacheBucket::FromString("FamilyA"), 'a');
	const auto B = MakeKey(FCacheBucket::FromString("FamilyB"), 'b');
	const auto Block = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{4}));
	const auto First = RecordWith(A, {Block, {}}), Second = RecordWith(B, {Block});
	ASSERT_TRUE(GetCache().Put({First})); ASSERT_TRUE(GetCache().Put({Second}));
	uint32 Blobs = 0;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(Directory.Root / "values/v1"))
		if (Entry.is_regular_file()) ++Blobs;
	EXPECT_EQ(Blobs, 2u); // One shared nonempty blob and one explicit empty blob.
	const auto Manifest = GetCacheStorage().Get({A, 4096, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	auto Descriptor = FCacheRecord::DecodeManifest(A, **Manifest); ASSERT_TRUE(Descriptor);
	EXPECT_FALSE(Descriptor->ToOutput(A));
	EXPECT_FALSE(FCacheRecord::DecodeManifest(B, **Manifest));
	EXPECT_FALSE(FCacheRecord::DecodeManifest(A, **Manifest, {.MaximumTotalBytes = 100}));
	EXPECT_FALSE(Descriptor->WithValues(Second.GetValues()));
	for (const auto& Record : {First, Second})
	{
		auto Loaded = GetCache().Get({Record.GetKey()}); ASSERT_TRUE(Loaded); ASSERT_TRUE(*Loaded);
		auto Warm = (**Loaded).ToOutput(Record.GetKey()); ASSERT_TRUE(Warm);
		ASSERT_EQ(Warm->GetValues().size(), Record.GetValues().size());
		for (size_t I = 0; I < Warm->GetValues().size(); ++I)
		{
			EXPECT_TRUE(Warm->GetValues()[I].Value.HasData());
			EXPECT_EQ(Warm->GetValues()[I].Value.GetRawHash(), Record.GetValues()[I].Value.GetRawHash());
			EXPECT_TRUE(std::ranges::equal(Warm->GetValues()[I].Value.GetData().GetBytes(), Record.GetValues()[I].Value.GetData().GetBytes()));
		}
		EXPECT_EQ(GetBuildMetadataPayload(*Warm).GetSize(), 2u);
		EXPECT_EQ(Warm->GetMessages()[0].Text, "fixture warning");
	}
}

TEST(FCacheTests, RejectsMissingOrDamagedBlobsAndRepairsThemOnPut)
{
	FScopedCacheDirectory Directory("ContentRepair");
	const auto Key = MakeKey(FCacheBucket::FromString("Repair"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCache().Put({Record}));
	const auto Blob = BlobKey(Record.GetValues()[0].Value);
	ASSERT_TRUE(GetCacheStorage().Put({Blob, Bytes({9}), 4096, ECacheStorageNamespace::Values}));
	auto Damaged = GetCache().Get({Key}); ASSERT_FALSE(Damaged); EXPECT_EQ(Damaged.error().Code, ECacheError::Corrupt);
	ASSERT_TRUE(GetCache().Put({Record})); EXPECT_TRUE(GetCache().Get({Key}).value().has_value());
	ASSERT_TRUE(std::filesystem::remove(EntryPath(Directory, Blob, "values/v1")));
	auto Missing = GetCache().Get({Key}); ASSERT_FALSE(Missing); EXPECT_EQ(Missing.error().Code, ECacheError::Corrupt);
	ASSERT_TRUE(GetCache().Put({Record})); EXPECT_TRUE(GetCache().Get({Key}).value().has_value());
}

TEST(FCacheTests, FailedPublicationRetainsOrphansAndExistingReaders)
{
	FScopedCacheDirectory Directory("ContentPublicationFailure");
	const auto ExistingKey = MakeKey(FCacheBucket::FromString("Existing"), 'a');
	const auto Existing = RecordWith(ExistingKey, {FSharedByteBuffer::Take(Bytes({9}))});
	ASSERT_TRUE(GetCache().Put({Existing}));
	const auto Key = MakeKey(FCacheBucket::FromString("Interrupted"), 'd');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	const auto Blocked = EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1");
	std::filesystem::create_directories(Blocked); // Fail the second blob after the first is published.
	const auto Failed = GetCache().Put({Record}); ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.error().Code, ECacheError::StorageFailure);
	EXPECT_FALSE(GetCache().Get({Key}).value().has_value());
	EXPECT_TRUE(std::filesystem::is_regular_file(EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1")));
	EXPECT_TRUE(GetCache().Get({ExistingKey}).value().has_value());
	const auto ManifestBlockedKey = MakeKey(FCacheBucket::FromString("ManifestBlocked"), 'e');
	std::filesystem::create_directories(EntryPath(Directory, ManifestBlockedKey, "records/v4"));
	EXPECT_FALSE(GetCache().Put({RecordWith(ManifestBlockedKey, {Existing.GetValues()[0].Value.GetData()})}));
	EXPECT_TRUE(GetCache().Get({ExistingKey}).value().has_value());
}

TEST(FCacheTests, EnforcesRawEncodedAndResidentBudgetsBeforePublishingManifest)
{
	FScopedCacheDirectory Directory("ContentBudgets");
	const auto Key = MakeKey(FCacheBucket::FromString("Budgets"), 'f');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{8}))});
	EXPECT_FALSE(GetCache().Put({Record, 8192, 100, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCache().Put({Record, 64, 8192, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCache().Put({Record, 8192, 8192, 4096}));
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{3}));
	EXPECT_FALSE(GetCache().Put({RecordWith(Key, {Backing.MakeView(0, 1)}), 8192, 8192, 4096}));
	EXPECT_FALSE(GetCache().Get({Key}).value().has_value());
	ASSERT_TRUE(GetCache().Put({Record}));
	const auto Manifest = GetCacheStorage().Get({Key, 8192, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	const auto Blob = GetCacheStorage().Get({BlobKey(Record.GetValues()[0].Value), 8192, ECacheStorageNamespace::Values}); ASSERT_TRUE(Blob); ASSERT_TRUE(*Blob);
	const auto AggregateLimit = (**Manifest).GetSize() + (**Blob).GetSize() - 1;
	EXPECT_FALSE(GetCache().Put({Record, AggregateLimit}));
	EXPECT_TRUE(GetCache().Get({Key}).value().has_value()); // Failed replacement preserves the published manifest.
	EXPECT_FALSE(GetCache().Get({Key, {}, AggregateLimit}));
	EXPECT_FALSE(GetCache().Get({Key, {.MaximumTotalBytes = 100}}));
	EXPECT_FALSE(GetCache().Get({Key, {}, 64}));
	EXPECT_FALSE(GetCache().Get({Key, {}, 8192, 4096}));
}

TEST(FCacheTests, ConcurrentContentWritersAndReadersSeeCompleteRecords)
{
	FScopedCacheDirectory Directory("ContentConcurrency");
	const auto Key = MakeKey(FCacheBucket::FromString("Concurrent"), '7');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{5}))});
	ASSERT_TRUE(GetCache().Put({Record}));
	std::latch Start(1); std::vector<std::thread> Threads;
	for (uint32 I = 0; I < 8; ++I) Threads.emplace_back([&] {
		Start.wait();
		for (uint32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			EXPECT_TRUE(GetCache().Put({Record}));
			auto Loaded = GetCache().Get({Key}); ASSERT_TRUE(Loaded); ASSERT_TRUE(*Loaded);
			EXPECT_TRUE((**Loaded).ToOutput(Key));
		}
	});
	Start.count_down(); for (auto& Thread : Threads) Thread.join();
}

TEST(FCacheTests, StoresAndLoadsValidatedStructuredRecords)
{
	FScopedCacheDirectory Directory("StructuredCacheReadWrite");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Records"), '7');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	ASSERT_TRUE(GetCache().Put({std::move(*Record), 4096}));

	auto Loaded = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	auto Warm = (**Loaded).ToOutput(Key); ASSERT_TRUE(Warm);
	const FValue* Value = Warm->FindValue(FValueId::FromName("Data"));
	ASSERT_NE(Value, nullptr);
	EXPECT_TRUE(std::ranges::equal(Value->GetData().GetBytes(), Bytes({1, 2, 3, 4})));
}

TEST(FCacheTests, RejectsCorruptStorageBeforeReturningARecord)
{
	FScopedCacheDirectory Directory("StructuredCacheCorruption");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Records"), '8');
	ASSERT_TRUE(GetCacheStorage().Put({Key, Bytes({9, 9, 9}), 4096, ECacheStorageNamespace::Records}));
	auto Loaded = GetCache().Get({Key, {}, 4096});
	ASSERT_FALSE(Loaded);
	EXPECT_EQ(Loaded.error().Code, ECacheError::Corrupt);
}

TEST(FCacheStorageTests, GetsAndAtomicallyReplacesCanonicalEntries)
{
	FScopedCacheDirectory Directory("CacheReadWrite");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer First = Bytes({1, 2, 3});
	const Durin::FByteBuffer Second = Bytes({4, 5});
	ASSERT_TRUE(Cache.Put({Key, First, 1024}));
	ASSERT_TRUE(Cache.Put({Key, Second, 1024}));

	const FCacheStorageGetResult Get = Cache.Get({Key, 1024});
	ASSERT_TRUE(Get);
	EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), Second));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Directory.Root / "Test" / "Objects" / "aa" / (std::string(32, 'a') + ".bin")));
}

TEST(FCacheStorageTests, NormalizesRootAndContainsBinaryKeyPaths)
{
	FScopedCacheDirectory Directory("CacheNormalizedRoot");
	FPaths::SetDerivedDataCacheDirForTests(
		(Directory.Root / "unused" / "..").generic_string());
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Nested/Objects");
	const FByteBuffer Value = Bytes({7, 8});
	for (const FXxHash128 Hash : {FXxHash128{1, 0}, FXxHash128{0, 1},
		FXxHash128{0xffffffffffffffffull, 0xffffffffffffffffull}})
	{
		const FCacheKey Key = FCacheKey::FromHash(Bucket, Hash);
		ASSERT_TRUE(Cache.Put({Key, Value, 1024}));
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_TRUE(Result);
		EXPECT_TRUE(std::ranges::equal((*Result)->GetBytes(), Value));
		const std::string Text = Key.ToString();
		EXPECT_TRUE(std::filesystem::is_regular_file(Directory.Root / "Test"
			/ "Nested" / "Objects" / Text.substr(0, 2) / (Text + ".bin")));
	}
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "unused"));
}

TEST(FCacheStorageTests, ValidatesRequestsAndBoundsValuesTransactionally)
{
	FScopedCacheDirectory Directory("CacheValidation");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	std::string Error;
	EXPECT_FALSE(FCacheBucket::FromString("../escape", &Error).IsValid());
	EXPECT_FALSE(FCacheBucket::FromString("/absolute", &Error).IsValid());
	EXPECT_FALSE(FCacheBucket::FromString(std::string(64, 'a'), &Error).IsValid());
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheBucket InternedBucket = FCacheBucket::FromString("Test/Objects");
	EXPECT_EQ(InternedBucket, Bucket);
	EXPECT_EQ(InternedBucket.ToString().data(), Bucket.ToString().data());
	EXPECT_FALSE(FCacheKey::FromString(Bucket, "ABC", &Error).IsValid());
	EXPECT_FALSE(FCacheKey::FromString(
		Bucket, std::string(32, '0'), &Error).IsValid());
	EXPECT_EQ(Error, "Cache key must not be the zero identity.");
	const FCacheKey EmptyKey = FCacheKey::FromHash(Bucket, {});
	EXPECT_FALSE(EmptyKey.IsValid());
	EXPECT_TRUE(EmptyKey.ToString().empty());
	const FXxHash128 Hash{0x0123456789abcdefull, 0xfedcba9876543210ull};
	const FCacheKey HashKey = FCacheKey::FromHash(Bucket, Hash);
	EXPECT_TRUE(HashKey.IsValid());
	EXPECT_EQ(HashKey.GetBucket(), Bucket);
	EXPECT_EQ(HashKey.GetHash(), Hash);
	EXPECT_EQ(FCacheKey::FromString(Bucket, HashKey.ToString()), HashKey);
	const FCacheKey Key = MakeKey(Bucket, 'b');
	const Durin::FByteBuffer Value = Bytes({1, 2, 3, 4});
	{
		const auto Result = Cache.Put({Key, Value, 3});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::ValueTooLarge);
	}
	{
		const auto Result = Cache.Get({Key, 3});
		ASSERT_TRUE(Result);
		EXPECT_FALSE(*Result);
	}
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "escape.bin"));
}

TEST(FCacheStorageTests, CacheKeyProxyPreservesBinaryIdentity)
{
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'd');
	const FCacheKeyProxy Proxy(Key);
	EXPECT_TRUE(Proxy.IsValid());
	EXPECT_EQ(*Proxy.AsCacheKey(), Key);
	EXPECT_EQ(Proxy.AsCacheKey()->GetBucket(), Bucket);
	EXPECT_EQ(Proxy.ToString(), Key.ToString());
	FCacheKeyProxy Copy = Proxy;
	EXPECT_EQ(Copy, Proxy);
	FCacheKeyProxy Moved = std::move(Copy);
	EXPECT_EQ(Moved, Proxy);
	Copy = Moved;
	EXPECT_EQ(Copy, Proxy);
	EXPECT_FALSE(FCacheKeyProxy{}.AsCacheKey()->IsValid());
	EXPECT_NE(Key, MakeKey(FCacheBucket::FromString("Test/Other"), 'd'));
}

TEST(FCacheStorageTests, BucketIsPartOfTheRecordIdentity)
{
	FScopedCacheDirectory Directory("CacheBucketIdentity");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheKey FirstKey = MakeKey(
		FCacheBucket::FromString("Test/First"), '8');
	const FCacheKey SecondKey = MakeKey(
		FCacheBucket::FromString("Test/Second"), '8');
	const FByteBuffer First = Bytes({1});
	const FByteBuffer Second = Bytes({2});
	ASSERT_TRUE(Cache.Put({FirstKey, First, 1}));
	ASSERT_TRUE(Cache.Put({SecondKey, Second, 1}));
	const auto FirstGet = Cache.Get({FirstKey, 1});
	const auto SecondGet = Cache.Get({SecondKey, 1});
	ASSERT_TRUE(FirstGet);
	ASSERT_TRUE(SecondGet);
	EXPECT_TRUE(std::ranges::equal((*FirstGet)->GetBytes(), First));
	EXPECT_TRUE(std::ranges::equal((*SecondGet)->GetBytes(), Second));
}

TEST(FCacheStorageTests, InternsBucketNamesAcrossThreads)
{
	std::vector<std::future<FCacheBucket>> Operations;
	for (uint32 Index = 0; Index < 16; ++Index)
		Operations.push_back(std::async(std::launch::async, [] {
			return FCacheBucket::FromString("Concurrent/SharedBucket");
		}));
	const FCacheBucket Expected = Operations.front().get();
	ASSERT_TRUE(Expected.IsValid());
	for (size_t Index = 1; Index < Operations.size(); ++Index)
	{
		const FCacheBucket Bucket = Operations[Index].get();
		EXPECT_EQ(Bucket, Expected);
		EXPECT_EQ(Bucket.ToString().data(), Expected.ToString().data());
	}
}

TEST(FCacheStorageTests, BucketIdentityRemainsStableAsRegistryGrows)
{
	const FCacheBucket Original = FCacheBucket::FromString("Stable/GrowingRegistry");
	ASSERT_TRUE(Original.IsValid());
	const char* Identity = Original.ToString().data();
	for (uint32 Index = 0; Index < 512; ++Index)
		ASSERT_TRUE(FCacheBucket::FromString(std::format("Stable/GrowingRegistry{}", Index)).IsValid());
	const FCacheBucket Repeated = FCacheBucket::FromString("Stable/GrowingRegistry");
	EXPECT_EQ(Original.ToString(), "Stable/GrowingRegistry");
	EXPECT_EQ(Original.ToString().data(), Identity);
	EXPECT_EQ(Repeated, Original);
	EXPECT_EQ(Repeated.ToString().data(), Identity);
}

TEST(FCacheTests, RejectsDamagedRawAndCompressedRecordsWithoutAStorageEnvelope)
{
	FScopedCacheDirectory Directory("CacheContentValidation");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Objects"), 'f');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	auto Raw = Record->Encode(4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	const auto Path = Directory.Root / "Test" / "Objects" / "ff"
		/ (std::string(32, 'f') + ".bin");
	for (const auto& Encoded : {*Raw, *Compressed})
	{
		ASSERT_TRUE(GetCacheStorage().Put({Key, Encoded, 4096}));
		auto Stored = FFileHelper::LoadFileToArray(Path); ASSERT_TRUE(Stored);
		EXPECT_TRUE(std::ranges::equal(*Stored, Encoded.GetBytes()));
		auto Loaded = FCacheRecord::Decode(Key, Encoded, {}, 4096); ASSERT_TRUE(Loaded);
		Stored->back() ^= std::byte{1};
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Stored, Path));
		auto Result = FCacheRecord::Decode(Key, FSharedByteBuffer::Take(*Stored), {}, 4096);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::Corrupt);
		EXPECT_FALSE(Result.error().Diagnostic.empty());
		FByteBuffer Truncated(Encoded.begin(), Encoded.end() - 1);
		ASSERT_TRUE(GetCacheStorage().Put({Key, Truncated, 4096}));
		auto Short = FCacheRecord::Decode(Key, FSharedByteBuffer::Copy(Truncated), {}, 4096); ASSERT_FALSE(Short);
		EXPECT_EQ(Short.error().Code, ECacheError::Corrupt);
		FByteBuffer Appended(Encoded.begin(), Encoded.end());
		Appended.push_back(std::byte{0});
		ASSERT_TRUE(GetCacheStorage().Put({Key, Appended, 4096}));
		auto Long = FCacheRecord::Decode(Key, FSharedByteBuffer::Copy(Appended), {}, 4096); ASSERT_FALSE(Long);
		EXPECT_EQ(Long.error().Code, ECacheError::Corrupt);
	}
}

TEST(FCacheTests, RejectsLegacyStorageEnvelopesAndReplacesThemOnPut)
{
	FScopedCacheDirectory Directory("CacheLegacyEnvelope");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Objects"), 'f');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	auto Raw = Record->Encode(4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	FBinaryWriter Legacy;
	Legacy.WriteHeader({0x43444444, 1, 1});
	Legacy.WriteU64(Compressed->GetSize());
	Legacy.WriteHash128(FXxHash128::HashBuffer(Compressed->GetBytes()));
	Legacy.WriteBytes(Compressed->GetBytes());
	ASSERT_TRUE(GetCacheStorage().Put({Key, Legacy.GetBytes(), 4096}));
	auto Rejected = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Rejected);
	EXPECT_FALSE(*Rejected); // Legacy namespaces are cold misses, never decoded.
	ASSERT_TRUE(GetCache().Put({*Record, 4096}));
	auto Loaded = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	EXPECT_TRUE((**Loaded).ToOutput(Key));
}

TEST(FCacheStorageTests, RejectsOversizedAndNonregularStoredEntries)
{
	FScopedCacheDirectory Directory("CacheStoredEntryValidation");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'c');
	const Durin::FByteBuffer Value(8, std::byte{1});
	ASSERT_TRUE(Cache.Put({Key, Value, 8}));
	{
		const auto Result = Cache.Get({Key, 4});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::ValueTooLarge);
	}
	const auto DirectoryEntry = Directory.Root / "Test" / "Objects" / "dd" / (std::string(32, 'd') + ".bin");
	std::filesystem::create_directories(DirectoryEntry);
	{
		const auto Result = Cache.Get({MakeKey(Bucket, 'd'), 8});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
}

TEST(FCacheStorageTests, ConcurrentSameKeyCallsPublishCompleteValues)
{
	FScopedCacheDirectory Directory("CacheConcurrency");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'e');
	const Durin::FByteBuffer First(128, std::byte{1});
	const Durin::FByteBuffer Second(128, std::byte{2});
	std::vector<std::thread> Threads;
	for (uint32 Index = 0; Index < 8; ++Index)
		Threads.emplace_back([&, Index] {
			const auto& Value = Index % 2 ? First : Second;
			EXPECT_TRUE(Cache.Put({Key, Value, 1024}));
			const FCacheStorageGetResult Get = Cache.Get({Key, 1024});
			ASSERT_TRUE(Get);
			EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), First)
				|| std::ranges::equal((*Get)->GetBytes(), Second));
		});
	for (std::thread& Thread : Threads) Thread.join();
}

TEST(FCacheStorageTests, PutNeverEvictsExistingEntries)
{
	FScopedCacheDirectory Directory("CacheNoRequestEviction");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/NoEviction");
	const Durin::FByteBuffer Value(64, std::byte{5});
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(Bucket,
			std::format("{:032x}", Index + 1));
		ASSERT_TRUE(Cache.Put({Key, Value, Value.size()}));
	}
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(Bucket,
			std::format("{:032x}", Index + 1));
		EXPECT_TRUE(Cache.Get({Key, Value.size()}));
	}
}

TEST(FCacheStorageTests, UnrelatedBucketsAndKeysMakeConcurrentProgress)
{
	FScopedCacheDirectory Directory("CacheParallelBuckets");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket FirstBucket = FCacheBucket::FromString("Test/First");
	const FCacheBucket SecondBucket = FCacheBucket::FromString("Test/Second");
	const Durin::FByteBuffer Value(4096, std::byte{7});
	std::latch Start(1);
	std::vector<std::future<bool>> Operations;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		Operations.push_back(std::async(std::launch::async, [&, Index] {
			Start.wait();
			const FCacheBucket& Bucket = Index % 2 ? FirstBucket : SecondBucket;
			const FCacheKey Key = FCacheKey::FromString(
				Bucket, std::format("{:032x}", Index + 1));
			if (!Cache.Put({Key, Value, Value.size()})) return false;
			const FCacheStorageGetResult Get = Cache.Get({Key, Value.size()});
			return Get && *Get && std::ranges::equal((*Get)->GetBytes(), Value);
		}));
	}
	Start.count_down();
	for (auto& Operation : Operations) EXPECT_TRUE(Operation.get());
}

TEST(FCacheStorageTests, BlockedStorageReturnsFailuresWithoutEscapingRoot)
{
	FScopedCacheDirectory Directory("CacheBlockedStorage");
	const std::filesystem::path Blocker = Directory.Root / "blocked";
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Blocker));
	const FCacheBucket Bucket = FCacheBucket::FromString("blocked/Bucket");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	{
		const auto Result = Cache.Put({MakeKey(Bucket, '1'), Value, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
	{
		const auto Result = Cache.Get({MakeKey(Bucket, '1'), 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
}

#if defined(_WIN32)
TEST(FCacheStorageTests, LockedEntryPreservesReadAndPublicationDiagnostics)
{
	FScopedCacheDirectory Directory("CacheLockedEntry");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_TRUE(Result);
		EXPECT_FALSE(*Result);
	}
	ASSERT_TRUE(Cache.Put({Key, Value, 1024}));
	const Durin::FFilePath Path = Directory.Root / "Test" / "Objects"
		/ "aa" / (std::string(32, 'a') + ".bin");
	const HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(File, INVALID_HANDLE_VALUE);
	{
		std::unique_ptr<void, decltype(&CloseHandle)> LockedFile(File, &CloseHandle);
		const auto Get = Cache.Get({Key, 1024});
		ASSERT_FALSE(Get);
		EXPECT_EQ(Get.error().Code, ECacheError::StorageFailure);
		EXPECT_NE(Get.error().Diagnostic.find("open for reading"), std::string::npos);
		EXPECT_NE(Get.error().Diagnostic.find(Path.generic_string()), std::string::npos);
		EXPECT_NE(Get.error().Diagnostic.find(std::format(":{}:", ERROR_SHARING_VIOLATION)), std::string::npos);
		const auto Put = Cache.Put({Key, Bytes({4, 5}), 1024});
		ASSERT_FALSE(Put);
		EXPECT_EQ(Put.error().Code, ECacheError::StorageFailure);
		EXPECT_NE(Put.error().Diagnostic.find("replace destination"), std::string::npos);
		EXPECT_NE(Put.error().Diagnostic.find(Path.generic_string()), std::string::npos);
	}
	const auto Get = Cache.Get({Key, 1024});
	ASSERT_TRUE(Get);
	EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), Value));
}
#endif

TEST(FCacheStorageTests, SymlinkEntriesAreNeverRead)
{
	FScopedCacheDirectory Directory("CacheSymlinkSafety");
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, '9');
	const Durin::FByteBuffer Value = Bytes({9, 8, 7});
	const std::filesystem::path Outside = Directory.Root / "outside.bin";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Outside));
	const std::filesystem::path Link = Directory.Root / "Test" / "Objects"
		/ "99" / (std::string(32, '9') + ".bin");
	std::filesystem::create_directories(Link.parent_path());
	std::error_code Error;
	std::filesystem::create_symlink(Outside, Link, Error);
	if (Error) GTEST_SKIP() << "Host cannot create a test symlink: " << Error.message();
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
	EXPECT_TRUE(std::filesystem::exists(Outside));
	EXPECT_TRUE(std::filesystem::is_symlink(Link));
}

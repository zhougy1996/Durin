#include "BuildServiceBackendPrivate.h"
#include "CacheRecordStorage.h"
#include "BuildCacheRecordPrivate.h"
#include "BuildSchedulerPrivate.h"
#include "CacheBackend.h"
#include "CacheRecordFixtureCodec.h"
#include "CacheStorageOwnership.h"
#include "CacheDiagnosticsPrivate.h"
#include "DerivedDataCache/DerivedDataCacheDiagnostics.h"
#include "DerivedDataCache/DerivedDataCacheMaintenance.h"
#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "CacheDispatchPrivate.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/DerivedDataCacheStorage.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/CompressedCacheValue.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "DerivedDataRequestOwner.h"
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
	auto SelectValues(std::span<const FValueId> Ids) -> FCacheRecordPolicy
	{
		FCacheRecordPolicyBuilder Builder(ECachePolicy::None);
		for (const auto& Id : Ids)
			Builder.AddValuePolicy(Id, ECachePolicy::Default);
		return Builder.Build();
	}

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
			Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
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
		return Durin::DerivedData::Private::MakeBuildCacheRecord(Key, std::move(Builder).Build().value()).value();
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
	class FAsyncFixtureCache final : public ICacheBackend
	{
	public:
		mutable uint32 Gets = 0, Puts = 0;
		mutable std::vector<FValueId> ObservedIds;
		bool Throw = false;
		auto Get(const FCacheGetBackendRequest& Request) const -> FCacheGetResult override
		{
			++Gets; if (Throw) throw std::runtime_error("fixture");
			ObservedIds.clear();
			for (const auto& Value : Request.Policy.GetValuePolicies())
				ObservedIds.push_back(Value.Id);
			if (Request.Stats) Request.Stats->ReadBytes = 17;
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult override
		{
			++Puts;
			return Durin::DerivedData::Private::ReadBuildOutput(Request.Record.GetKey(), Request.Record).transform([](auto&&) {});
		}
	};
}

TEST(FAsyncCacheTests, CanceledQueuedOwnerCompletesWithoutOtherOwnerProgress)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner First, Second;
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '1')}};
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
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '2')}};
	uint32 Calls = 0;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; }));
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Scheduler->Work.empty()); EXPECT_TRUE(Owner.Poll());
}

TEST(FAsyncCacheTests, BatchCancellationDoesNotCancelSharedOwner)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>(); auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("ScopeCancel"), '5')}};
	FRequestOwner Busy, Owner;
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	std::atomic<uint32> Canceled = 0; uint32 Other = 0;
	std::shared_ptr<IBuildScheduledWork> Batch;
	auto Submit = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
		auto Accepted = Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Canceled; });
		EXPECT_TRUE(Accepted); if (Accepted) Batch = *Accepted;
	});
	EXPECT_EQ(Submit.Wait().WaitStatus, ETaskWaitStatus::Completed);
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Other; }));
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
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("CancelQueue"), '3')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	uint32 Calls = 0;
	{ FRequestOwner Owner; ASSERT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; })); }
	EXPECT_EQ(Calls, 1u); EXPECT_FALSE(Busy.Poll()); Scheduler->Run();
}

TEST(FAsyncCacheTests, RequestOwnerWaitsForNativeDispatchCompletion)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FTailScheduler final : public IBuildScheduler
	{
	public:
		class FWork final : public IBuildScheduledWork, public Durin::DerivedData::Private::INativeBuildWork
		{
		public:
			Tasks::FTask Handle;
			auto GetTaskHandle() const -> Tasks::FTask override { return Handle; }
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
			FTaskLaunchOptions Options;
			auto Admission = Durin::Private::LaunchCancelableTaskWithCompletion("DDCNativeTail",
				[this, Work = std::move(Work), Capture = std::exchange(Capture, {})](const FTaskCancellationToken&) {
					Work(); Entered.Trigger(); Release.Wait();
				}, {}, Options);
			Scheduled->Handle = std::move(Admission); return Scheduled;
		}
	};
	auto Scheduler = std::make_shared<FTailScheduler>(); std::weak_ptr<int> Weak = Scheduler->Capture;
	auto Operation = std::make_shared<FRequestOwner>(); auto& Owner = *Operation; std::atomic<uint32> Calls = 0;
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("NativeTail"), '6')}};
	FThreadEvent Submitted, WaitStarted, WaitReturned;
	auto Producer = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Calls; }));
		Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_TRUE(Scheduler->Entered.WaitFor(5.0));
	EXPECT_EQ(Calls.load(), 1u); EXPECT_FALSE(Owner.Poll()); EXPECT_FALSE(Weak.expired());
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed); EXPECT_FALSE(Owner.Poll());
	auto Waiter = Tasks::LaunchTask("DDCOwnerJoin", [&] {
		WaitStarted.Trigger(); EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); WaitReturned.Trigger();
	});
	EXPECT_TRUE(WaitStarted.WaitFor(5.0)); EXPECT_FALSE(WaitReturned.WaitFor(0.01));
	Scheduler->Release.Trigger();
	EXPECT_EQ(Waiter.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_TRUE(Owner.Poll()); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(WaitReturned.WaitFor(1.0));
}

TEST(FAsyncCacheTests, RequestOwnerQueuedCancellationNeedsNoBusyOwnerProgress)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>(); auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	FRequestOwner Busy; auto Operation = std::make_shared<FRequestOwner>(); auto& Owner = *Operation;
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("NestedCancel"), '7')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted; std::atomic<uint32> Calls = 0;
	auto Producer = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
		EXPECT_TRUE(Async->Get(Requests, Owner, [&](auto, auto Result, auto) {
			EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::Canceled); ++Calls;
		}));
		Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_FALSE(Owner.Poll());
	Owner.Cancel(); EXPECT_EQ(Calls.load(), 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_FALSE(Busy.Poll());
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	ASSERT_EQ(Scheduler->Work.size(), 1u); Scheduler->Run(); EXPECT_EQ(Backend->Gets, 1u);
}

TEST(FAsyncCacheTests, RequestOwnerCancellationWaitsForCallbackAndRejectsSelfWait)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FRequestOwner>();
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("OperationCancel"), '8')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted, CallbackEntered, ReleaseCallback, CancelReturned;
	auto Producer = Tasks::LaunchTask("RequestOwnerProducer", [&] {
		EXPECT_TRUE(Async->Get(Requests, *Operation, [&](auto, auto Result, auto) {
			EXPECT_FALSE(Result);
			EXPECT_EQ(Operation->Wait(), ERequestWaitResult::WouldBlock);
			CallbackEntered.Trigger(); ReleaseCallback.Wait();
		}));
		Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0));
	auto Cancel = Tasks::LaunchTask("RequestOwnerCancel", [&] { Operation->Cancel(); CancelReturned.Trigger(); });
	EXPECT_TRUE(CallbackEntered.WaitFor(5.0));
	EXPECT_FALSE(CancelReturned.WaitFor(0.01));
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed); EXPECT_FALSE(Operation->Poll());
	ReleaseCallback.Trigger();
	EXPECT_EQ(Cancel.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_TRUE(Operation->Poll()); EXPECT_FALSE(Busy.Poll());
	Scheduler->Run();
}

TEST(FAsyncCacheTests, RequestOwnerUndispatchedRecordDoesNotRetainCoreShutdown)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	const auto Before = GetTaskSchedulerDiagnostics().NonterminalTaskCount;
	auto Operation = std::make_shared<FRequestOwner>();
	ASSERT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, Before);
	ShutdownTaskSystem(ETaskShutdownMode::Cancel);
	Operation.reset();
	EXPECT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, 0u);
}

TEST(FAsyncCacheTests, RequestOwnerDestructionCancelsQueuedRequestWithoutRetainingFacade)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FRequestOwner>();
	std::weak_ptr<FRequestOwner> Weak = Operation;
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("OperationDestruction"), 'c')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	uint32 Calls = 0;
	auto Batch = Async->Get(Requests, *Operation, [&](auto, auto Result, auto) { EXPECT_FALSE(Result); ++Calls; });
	ASSERT_TRUE(Batch);
	Operation.reset();
	EXPECT_TRUE(Weak.expired()); EXPECT_EQ(Calls, 1u);
	EXPECT_FALSE(Busy.Poll()); Scheduler->Run();
}

TEST(FAsyncCacheTests, RequestOwnerInlineOwnerPollingDoesNotCloseProducerSubmission)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), CreateInlineBuildScheduler(), {.Concurrency = 1});
	auto Operation = std::make_shared<FRequestOwner>();
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("OperationInline"), '9')}};
	uint32 Calls = 0;
	auto Producer = Tasks::LaunchTask("RequestOwnerInline", [&] {
		for (uint32 Index = 0; Index != 2; ++Index)
		{
			EXPECT_TRUE(Async->Get(Requests, *Operation, [&](auto, auto, auto) { ++Calls; }));
			EXPECT_TRUE(Operation->Poll());
		}
	});
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls, 2u); EXPECT_TRUE(Operation->Poll());
}

TEST(FAsyncCacheTests, RequestOwnerWaitsForNativeHandlePublishedAfterSynchronousCallback)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FPublicationScheduler final : public IBuildScheduler
	{
	public:
		class FWork final : public IBuildScheduledWork, public Durin::DerivedData::Private::INativeBuildWork
		{
		public:
			Tasks::FTask Handle;
			auto GetTaskHandle() const -> Tasks::FTask override { return Handle; }
			auto Wait() const -> ERequestWaitResult override
			{ return WaitTask(Handle).WaitStatus == ETaskWaitStatus::Completed ? ERequestWaitResult::Completed : ERequestWaitResult::WouldBlock; }
		};
		std::function<void()> BusyWork;
		FRequestOwner* Owner = nullptr;
		FThreadEvent TailEntered, ReleaseTail;
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			if (!Owner) { BusyWork = std::move(Work); return std::shared_ptr<IBuildScheduledWork>{}; }
			Work();
			EXPECT_FALSE(Owner->Poll()); // No native handle has been published yet.
			auto Scheduled = std::make_shared<FWork>();
			FTaskLaunchOptions Options;
			auto Native = Durin::Private::LaunchCancelableTaskWithCompletion("OperationDispatchTail",
				[this, Work = std::move(Work)](const FTaskCancellationToken&) { TailEntered.Trigger(); ReleaseTail.Wait(); }, {}, Options);
			Scheduled->Handle = std::move(Native); return Scheduled;
		}
	};
	auto Scheduler = std::make_shared<FPublicationScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FRequestOwner>();
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("OperationPublication"), 'a')}};
	ASSERT_TRUE(Async->Get(Requests, Busy, [](auto, auto, auto) {}));
	FThreadEvent Submitted;
	auto Producer = Tasks::LaunchTask("OperationLateDispatch", [&] {
		EXPECT_TRUE(Async->Get(Requests, *Operation, [](auto, auto, auto) {}));
		Submitted.Trigger();
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); Scheduler->Owner = Operation.get();
	std::exchange(Scheduler->BusyWork, {})();
	EXPECT_TRUE(Scheduler->TailEntered.WaitFor(5.0));
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed); EXPECT_FALSE(Operation->Poll());
	Scheduler->ReleaseTail.Trigger();
	EXPECT_EQ(Producer.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Operation->Wait(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, RequestOwnerExceptionalProducerStillJoinsQueuedRetirement)
{
	EnsureCacheTaskGameThread();
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Async = CreateAsyncCache(std::make_shared<FAsyncFixtureCache>(), Scheduler, {.Concurrency = 1});
	FRequestOwner Busy;
	auto Operation = std::make_shared<FRequestOwner>();
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("OperationException"), 'b')}};
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
			Submitted.Trigger(); throw;
		}
	});
	EXPECT_TRUE(Submitted.WaitFor(5.0)); EXPECT_FALSE(Operation->Poll());
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
	std::vector<FCacheGetBackendRequest> Gets(4096, FCacheGetBackendRequest{.Key = Key});
	std::vector<uint32> Calls(Gets.size());
	auto Read = Async->Get(Gets, Owner, [&](uint32 Index, auto Result, auto) {
		EXPECT_TRUE(Result); ASSERT_LT(Index, Calls.size()); ++Calls[Index];
	});
	ASSERT_TRUE(Read); EXPECT_EQ((*Read)->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Scheduler->MaximumDepth, 1u); EXPECT_EQ(Backend->Gets, 4096u);
	for (auto Count : Calls) EXPECT_EQ(Count, 1u);
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2}))});
	std::vector<FCachePutBackendRequest> Puts(4096, FCachePutBackendRequest{Record});
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
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("InlineNested"), 'b')}};
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
	const std::array Writes{FCachePutBackendRequest{Record}};
	const std::array Batch{FCachePutBackendRequest{Record}, FCachePutBackendRequest{Record}};
	uint32 Calls = 0;
	auto TooLarge = Async->Put(Batch, Rejected, [&](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(TooLarge); EXPECT_EQ(TooLarge.error().Code, ECacheError::Capacity);
	EXPECT_TRUE(Rejected.Poll()); EXPECT_TRUE(Scheduler->Work.empty());
	const std::array Reads{FCacheGetBackendRequest{.Key = Key}};
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
	const std::array Sliced{FCachePutBackendRequest{RecordWith(Key, {Backing.MakeView(0, 1)})}};
	auto Slice = Async->Put(Sliced, Rejected, [&](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Slice); EXPECT_EQ(Slice.error().Code, ECacheError::Capacity); EXPECT_TRUE(Rejected.Poll());
}

TEST(FAsyncCacheTests, RetainedBytesRemainChargedDuringCallbackAndRecoverAfterward)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .MaximumRetainedBytes = 14000});
	FRequestOwner Owner, Other;
	const auto Key = MakeKey(FCacheBucket::FromString("RetainedCallback"), 'd');
	const std::array Requests{FCachePutBackendRequest{RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{5}))})}};
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
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("FairQueue"), 'e')}};
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

TEST(FAsyncCacheTests, RetainedAdmissionDeduplicatesRecordBackingAndChargesPolicyOverrides)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .MaximumRetainedBytes = 14000});
	FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("RetainedTables"), 'a');
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{6}));
	const std::array Writes{FCachePutBackendRequest{RecordWith(Key, {Backing, Backing})}};
	ASSERT_TRUE(Async->Put(Writes, Owner, [](auto, auto Result) { EXPECT_TRUE(Result); }));
	Scheduler->Run(); EXPECT_EQ(Backend->Puts, 1u);
	std::vector<FValueId> Ids; Ids.reserve(4096);
	for (uint32 Index = 0; Index < 4096; ++Index) Ids.push_back(FValueId::FromName("Selection").MakeIndexed(Index));
	const std::array Reads{FCacheGetBackendRequest{.Key = Key, .Policy = SelectValues(Ids)}};
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
	const std::array Requests{FCacheGetBackendRequest{.Key = Key}};
	std::vector<FCacheGetBackendRequest> Waiting(4094, Requests[0]);
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
	const auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(MakeKey(FCacheBucket::FromString("QueuedData"), 'b'), std::move(Builder).Build().value()).value();
	const uint64 Bytes = Record.GetMeta().GetBytes().GetRetainedCapacityBytes();
	const std::array Writes{FCachePutBackendRequest{Record}};
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
	{ return {.Policy = EBuildPolicy::Build}; }
}

TEST(FResourceBuildSessionTests, InlineNestedBuildAndTypedAdmissionSurviveCloseAndReopen)
{
	AssetPrivate::FResourceBuildSessions Sessions;
	const auto Definition = ResourceDefinition();
	auto Session = Sessions.Get(EBuildCacheExecution::Inline); ASSERT_TRUE(Session);
	FRequestOwner Owner; uint32 Calls = 0;
	(*Session)->Build(Definition, Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls;
		auto Nested = Sessions.Build(Definition, {}, ResourceOptions());
		ASSERT_TRUE(Nested); EXPECT_EQ(Nested->GetStatus(), EStatus::Ok); ++Calls; }, {}, ResourceOptions());
	EXPECT_EQ(Calls, 2u); EXPECT_TRUE(Owner.Poll());
	auto Unknown = Sessions.Build(std::move(FBuildDefinitionBuilder("Resource.Unknown")).Build().value(), {}, ResourceOptions());
	ASSERT_TRUE(Unknown);
	EXPECT_EQ(Unknown->GetStatus(), EStatus::Error);
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
	(*Session)->Build(Definition, Owner, [&](auto) {
		EXPECT_EQ(Sessions.Close(), EBuildDrainResult::WouldBlock);
		auto Closed = Sessions.Get(EBuildCacheExecution::Inline);
		ASSERT_FALSE(Closed); EXPECT_EQ(Closed.error().Reason, EBuildAdmissionReason::Closed); }, {}, ResourceOptions());
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
		auto Service = Durin::DerivedData::Private::CreateBuildForBackend();
		require(Service->Register(std::make_shared<FResourceSessionFunction>()));
		return Service;
	};
	ASSERT_EQ(Sessions.SetTaskService(MakeService()), EBuildDrainResult::Drained);
	auto Old = Sessions.Get(EBuildCacheExecution::Task); ASSERT_TRUE(Old);
	FRequestOwner Owner;
	FThreadEvent Entered, Release; std::atomic<bool> Reenter = true;
	struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } ReleaseOnExit{Release};
	(*Old)->Build(Definition, Owner, [&](auto) {
		Entered.Trigger(); Release.Wait();
		if (Reenter) EXPECT_TRUE(Sessions.Get(EBuildCacheExecution::Task)); }, {}, ResourceOptions());
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
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("CapacityCode"), 'f')}};
	Backend->Throw = true;
	ASSERT_TRUE(Async->Get(Requests, Owner, [](auto, auto Result, auto) {
		ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}));
	auto Overflow = Async->Get(Requests, Other, [](auto, auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Overflow); EXPECT_EQ(Overflow.error().Code, ECacheError::Capacity); EXPECT_TRUE(Other.Poll());
	Scheduler->Run();
	auto ZeroBytes = CreateAsyncCache(Backend, Scheduler, {.MaximumRetainedBytes = 0});
	// A uniform-policy Get retains no queued data; fixed overhead uses item admission.
	ASSERT_TRUE(ZeroBytes->Get(Requests, Other, [](auto, auto, auto) {}));
	Scheduler->Run(); EXPECT_TRUE(Other.Poll());
	const std::array Writes{FCachePutBackendRequest{RecordWith(Requests[0].Key, {FSharedByteBuffer::Take(Bytes({1}))})}};
	auto Rejected = ZeroBytes->Put(Writes, Other, [](auto, auto) { ADD_FAILURE(); });
	ASSERT_FALSE(Rejected); EXPECT_EQ(Rejected.error().Code, ECacheError::Capacity); EXPECT_TRUE(Other.Poll());
}

TEST(FAsyncCacheTests, OwnsBatchPoliciesAndBoundsConcurrencyAndAdmission)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 3}); FRequestOwner Owner(ERequestPriority::High);
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'a');
	std::vector<FValueId> Ids{FValueId::FromName("Selected")}; const auto Original = Ids;
	std::array Requests{FCacheGetBackendRequest{.Key = Key, .Policy = SelectValues(Ids)}, FCacheGetBackendRequest{.Key = Key}};
	uint32 Callbacks = 0; uint32 Indices = 0; FCacheQueryStats Borrowed; Requests[0].Stats = &Borrowed;
	ASSERT_TRUE(Async->Get(Requests, Owner, [&](uint32 Index, auto Result, auto Stats) { EXPECT_TRUE(Result); EXPECT_EQ(Stats.ReadBytes, 17u); ++Callbacks; Indices |= 1 << Index; }));
	Ids.clear(); Requests = {}; EXPECT_FALSE(Owner.Poll()); EXPECT_EQ(Scheduler->Work.size(), 1u);
	std::array Extra{FCacheGetBackendRequest{Key}, FCacheGetBackendRequest{Key}};
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
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'e');
	const std::array Requests{FCacheGetBackendRequest{Key}, FCacheGetBackendRequest{Key}};
	ASSERT_TRUE(Async->Get(Requests, Low, [](auto, auto, auto) {})); ASSERT_TRUE(Async->Get(Requests, High, [](auto, auto, auto) {}));
	while (!Scheduler->Work.empty()) Scheduler->Run();
	EXPECT_EQ(Scheduler->Priorities, (std::vector{ERequestPriority::Low, ERequestPriority::High, ERequestPriority::High, ERequestPriority::Low}));
	auto Outer = CreateAsyncCache(Backend, CreateInlineBuildScheduler()); auto Inner = CreateAsyncCache(Backend, CreateInlineBuildScheduler());
	FRequestOwner OuterOwner, InnerOwner;
	uint32 Callbacks = 0;
	const std::array One{FCacheGetBackendRequest{Key}};
	ASSERT_TRUE(Outer->Get(One, OuterOwner, [&](auto, auto, auto) {
		ASSERT_TRUE(Inner->Get(One, InnerOwner, [&](auto, auto, auto) { EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::WouldBlock); EXPECT_EQ(Outer->Drain(), ERequestWaitResult::WouldBlock); ++Callbacks; }));
		++Callbacks;
	}));
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(OuterOwner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(InnerOwner.Wait(), ERequestWaitResult::Completed);
}

TEST(FAsyncCacheTests, CancellationDropsExceptionsAndCallbackRetirementCompleteExactlyOnce)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1});
	const std::array Requests{FCacheGetBackendRequest{MakeKey(FCacheBucket::FromString("Async"), 'b')}};
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
	std::array Requests{FCachePutBackendRequest{RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2}))})}};
	uint32 Callbacks = 0;
	ASSERT_TRUE(Async->Put(Requests, Owner, [&](auto, auto Result) { EXPECT_TRUE(Result); ++Callbacks; }));
	Requests = {}; Scheduler->Run(); EXPECT_EQ(Backend->Puts, 1u);
	const std::array Reads{FCacheGetBackendRequest{Key}, FCacheGetBackendRequest{Key}};
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
	const std::array Reads{FCacheGetBackendRequest{MakeKey(FCacheBucket::FromString("Async"), 'e')}};
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

TEST(FAsyncCacheTests, CoreShutdownDrainsRunningAndPendingAcceptedRequests)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FBlockingCache final : public ICacheBackend
	{
	public:
		std::promise<void> Entered;
		std::shared_future<void> Release;
		mutable std::atomic<uint32> Gets = 0;
		auto Get(const FCacheGetBackendRequest&) const -> FCacheGetResult override
		{ if (++Gets == 1) const_cast<FBlockingCache*>(this)->Entered.set_value(); Release.wait(); return std::optional<FCacheRecord>{}; }
		auto Put(const FCachePutBackendRequest&) const -> FCachePutResult override { return {}; }
	};
	std::promise<void> Release; auto Cache = std::make_shared<FBlockingCache>(); Cache->Release = Release.get_future().share();
	auto Entered = Cache->Entered.get_future(); auto Async = CreateAsyncCache(Cache, {}, {.Concurrency = 1}); FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("Async"), 'd');
	const std::array Requests{FCacheGetBackendRequest{Key}, FCacheGetBackendRequest{Key}};
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
	const auto Original = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1}))});
	ASSERT_TRUE(GetCacheBackend().Put({Original}));
	const auto Replacement = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({2})), FSharedByteBuffer::Take(Bytes({3}))});
	for (uint32 StopAt : {1u, 2u, 3u, 4u, 5u, 6u})
	{
		uint32 Checks = 0;
		auto Saved = GetCacheBackend().Put({.Record = Replacement, .ShouldCancel = [&] { return ++Checks == StopAt; }});
		ASSERT_FALSE(Saved); EXPECT_EQ(Saved.error().Code, ECacheError::Canceled);
		auto Found = GetCacheBackend().Get({Key});
		ASSERT_TRUE(Found && *Found);
		EXPECT_EQ((**Found).GetValues().size(), 1u);
		EXPECT_EQ((**Found).GetValues()[0].Value.GetRawHash(), Original.GetValues()[0].Value.GetRawHash());
	}
}

TEST(FCacheRecordPolicyTests, DefaultsAndOwnedOverridesAreCanonical)
{
	const auto A = FValueId::FromName("PolicyA"), B = FValueId::FromName("PolicyB");
	FCacheRecordPolicy Default;
	EXPECT_TRUE(Default.IsDefault());
	EXPECT_TRUE(Default.IsUniform());
	EXPECT_EQ(Default.GetValuePolicy(A), ECachePolicy::Default);
	FCacheRecordPolicy Descriptors(ECachePolicy::Query | ECachePolicy::SkipData);
	EXPECT_EQ(Descriptors.GetRecordPolicy(), ECachePolicy::Query);
	EXPECT_EQ(Descriptors.GetValuePolicy(A), ECachePolicy::Query | ECachePolicy::SkipData);
	FCacheRecordPolicy Policy;
	{
		FCacheRecordPolicyBuilder Builder(ECachePolicy::None);
		Builder.AddValuePolicy(B, ECachePolicy::Query | ECachePolicy::SkipData);
		Builder.AddValuePolicy(A, ECachePolicy::Default);
		Policy = Builder.Build();
	}
	EXPECT_EQ(Policy.GetBasePolicy(), ECachePolicy::None);
	EXPECT_TRUE(HasCachePolicy(Policy.GetRecordPolicy(), ECachePolicy::Query));
	EXPECT_TRUE(std::ranges::is_sorted(Policy.GetValuePolicies(), {}, &FCacheValuePolicy::Id));
	EXPECT_EQ(Policy.GetValuePolicy(B), ECachePolicy::Query | ECachePolicy::SkipData);
	EXPECT_EQ(Policy.GetValuePolicy(FValueId::FromName("Unspecified")), ECachePolicy::None);
	auto Copy = Policy;
	Policy = ECachePolicy::None;
	EXPECT_EQ(Copy.GetValuePolicy(A), ECachePolicy::Default);
	EXPECT_FALSE(Copy.IsUniform());
	FCacheRecordPolicyBuilder Redundant;
	Redundant.AddValuePolicy(A, ECachePolicy::Default);
	EXPECT_TRUE(Redundant.Build().IsDefault());
}

TEST(FCacheRecordPolicyDeathTests, InvalidPoliciesAndConsumedBuildersAreRejected)
{
	EXPECT_DEATH({ FCacheRecordPolicy Policy(static_cast<ECachePolicy>(0x80)); }, "Unsupported cache policy");
	EXPECT_DEATH({ FCacheRecordPolicyBuilder Builder; Builder.AddValuePolicy({}, ECachePolicy::Default); }, "valid Value ID");
	EXPECT_DEATH({
		FCacheRecordPolicyBuilder Builder;
		const auto Id = FValueId::FromName("Duplicate");
		Builder.AddValuePolicy(Id, ECachePolicy::None); Builder.AddValuePolicy(Id, ECachePolicy::SkipData); Builder.Build(); }, "Duplicate cache value policy");
	EXPECT_DEATH({ FCacheRecordPolicyBuilder Builder; Builder.Build(); Builder.Build(); }, "no longer usable");
	EXPECT_DEATH({
		FCacheRecordPolicyBuilder Builder; Builder.Build();
		Builder.AddValuePolicy(FValueId::FromName("AfterBuild"), ECachePolicy::Default); }, "no longer usable");
}

TEST(FCacheSelectionTests, DefaultAndValueOverridesDistinguishSkippedDataFromDisabledQueries)
{
	FScopedCacheDirectory Directory("cache-value-policy");
	const auto Key = MakeKey(FCacheBucket::FromString("Policy"), 'b');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2})), FSharedByteBuffer::Take(Bytes({3}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto Values = Record.GetValues();
	FCacheRecordPolicyBuilder Builder;
	Builder.AddValuePolicy(Values[1].Id, ECachePolicy::Query | ECachePolicy::SkipData);
	Builder.AddValuePolicy(Values[2].Id, ECachePolicy::None);
	const auto Policy = Builder.Build();
	std::filesystem::remove(EntryPath(Directory, BlobKey(Values[2].Value), "values/v1"));
	FCacheQueryStats Stats;
	auto Loaded = GetCacheBackend().Get({.Key = Key, .Policy = Policy, .Stats = &Stats});
	ASSERT_TRUE(Loaded && *Loaded);
	EXPECT_EQ(Stats.ValuesRead, 1u);
	EXPECT_TRUE((**Loaded).FindValue(Values[0].Id)->HasData());
	EXPECT_FALSE((**Loaded).FindValue(Values[1].Id)->HasData());
	EXPECT_FALSE((**Loaded).FindValue(Values[2].Id)->HasData());
	const auto SkippedPath = EntryPath(Directory, BlobKey(Values[1].Value), "values/v1");
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({9}), SkippedPath));
	// Presence is checked, but skipped data is neither read nor integrity-validated.
	ASSERT_TRUE(GetCacheBackend().Get({.Key = Key, .Policy = Policy}));
	std::filesystem::remove(SkippedPath);
	auto Missing = GetCacheBackend().Get({.Key = Key, .Policy = Policy});
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().Code, ECacheError::MissingValue);
	std::filesystem::create_directory(SkippedPath);
	auto InvalidEntry = GetCacheBackend().Get({.Key = Key, .Policy = Policy});
	ASSERT_FALSE(InvalidEntry);
	EXPECT_EQ(InvalidEntry.error().Code, ECacheError::StorageFailure);
	auto Disabled = GetCacheBackend().Get({.Key = Key, .Policy = ECachePolicy::None, .Stats = &Stats});
	ASSERT_TRUE(Disabled);
	EXPECT_FALSE(*Disabled);
	EXPECT_EQ(Stats.ReadBytes, 0u);
}

TEST(FCacheSelectionTests, MetadataAndSelectionLeaveOtherValuesAsReferences)
{
	FScopedCacheDirectory Directory("cache-selection");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{1})),
		FSharedByteBuffer::Take(Bytes({2, 3})), FSharedByteBuffer{}});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	FCacheQueryStats Stats;
	auto Metadata = GetCacheBackend().Get({.Key = Key, .Policy = ECachePolicy::Query | ECachePolicy::SkipData, .Stats = &Stats});
	ASSERT_TRUE(Metadata && *Metadata); EXPECT_EQ(Stats.ValuesRead, 0u); EXPECT_EQ(Stats.DecodedBytes, 0u);
	EXPECT_EQ(Stats.ReadBytes, Durin::DerivedData::Private::EncodeCacheManifest(Record)->GetSize());
	EXPECT_EQ(GetBuildMetadataPayload(**Metadata).GetSize(), 2u);
	EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**Metadata)));
	for (const auto& Item : (**Metadata).GetValues()) EXPECT_FALSE(Item.Value.HasData());
	const std::array Selected{FValueId::FromName("Block").MakeIndexed(2), FValueId::FromName("Block").MakeIndexed(1)};
	auto Partial = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected), .Stats = &Stats});
	ASSERT_TRUE(Partial && *Partial); EXPECT_EQ(Stats.ValuesRead, 2u); EXPECT_EQ(Stats.DecodedBytes, 2u);
	EXPECT_FALSE((**Partial).FindValue(FValueId::FromName("Block").MakeIndexed(0))->HasData());
	EXPECT_TRUE((**Partial).FindValue(Selected[0])->HasData()); // Empty data remains resident.
	EXPECT_TRUE((**Partial).FindValue(Selected[1])->HasData());
	EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**Partial)));
	auto Full = GetCacheBackend().Get({Key});
	ASSERT_TRUE(Full && *Full);
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**Full)));
	std::vector<FValueId> All;
	for (const auto& Value : Record.GetValues()) All.push_back(Value.Id);
	auto AllSelected = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(All)});
	ASSERT_TRUE(AllSelected && *AllSelected);
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**AllSelected)));
	auto EmptySelected = GetCacheBackend().Get({.Key = Key, .Policy = ECachePolicy::Query | ECachePolicy::SkipData, .Stats = &Stats});
	ASSERT_TRUE(EmptySelected && *EmptySelected); EXPECT_EQ(Stats.ValuesRead, 0u);
}

TEST(FCacheSelectionTests, OverridesIgnoreAbsentIdsButQueriedBlobsMustExist)
{
	FScopedCacheDirectory Directory("cache-selection-errors");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'b');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	const std::array Selected{Record.GetValues()[0].Id};
	auto Miss = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected)});
	ASSERT_TRUE(Miss); EXPECT_FALSE(*Miss);
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const std::array Unknown{FValueId::FromName("Unknown")}; FCacheQueryStats Stats;
	auto Missing = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Unknown), .Stats = &Stats});
	ASSERT_TRUE(Missing && *Missing);
	EXPECT_EQ(Stats.ValuesRead, 0u);
	for (const auto& Value : (**Missing).GetValues())
		EXPECT_FALSE(Value.Value.HasData());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({9}), EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1")));
	auto Undamaged = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected)});
	ASSERT_TRUE(Undamaged && *Undamaged);
	EXPECT_FALSE(GetCacheBackend().Get({Key}));
	std::filesystem::remove(EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1"));
	auto Partial = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected)});
	ASSERT_TRUE(Partial && *Partial);
	EXPECT_FALSE(GetCacheBackend().Get({Key}));
	std::filesystem::remove(EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1"));
	Missing = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected)});
	ASSERT_FALSE(Missing); EXPECT_EQ(Missing.error().Code, ECacheError::MissingValue);
	auto Metadata = GetCacheBackend().Get({.Key = Key, .Policy = ECachePolicy::Query | ECachePolicy::SkipData});
	ASSERT_FALSE(Metadata);
	EXPECT_EQ(Metadata.error().Code, ECacheError::MissingValue);
}

TEST(FCacheSelectionTests, EnforcesSelectedBudgetsAndCooperativeCancellation)
{
	FScopedCacheDirectory Directory("cache-selection-budget");
	const auto Key = MakeKey(FCacheBucket::FromString("Selection"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(1024 * 1024, std::byte{1})), FSharedByteBuffer::Take(Bytes({2}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const std::array Selected{FValueId::FromName("Block").MakeIndexed(1)};
	// Logical output bounds still validate every descriptor, not only resident values.
	EXPECT_FALSE(GetCacheBackend().Get({.Key = Key, .RecordLimits = {.MaximumTotalBytes = 1024}, .Policy = SelectValues(Selected)}));
	FCacheQueryStats Stats;
	auto Partial = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected), .Stats = &Stats});
	ASSERT_TRUE(Partial && *Partial);
	EXPECT_TRUE(GetCacheBackend().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes, .MaximumWorkingSetBytes = Stats.PeakWorkingSetBytes, .Policy = SelectValues(Selected)}));
	EXPECT_FALSE(GetCacheBackend().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes - 1, .Policy = SelectValues(Selected)}));
	EXPECT_FALSE(GetCacheBackend().Get({.Key = Key, .MaximumWorkingSetBytes = Stats.PeakWorkingSetBytes}));
	uint32 TotalChecks = 0;
	ASSERT_TRUE(GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected), .ShouldCancel = [&] { ++TotalChecks; return false; }}));
	bool SawDecodedCancellation = false;
	for (uint32 StopAt = 1; StopAt <= TotalChecks; ++StopAt)
	{
		uint32 Checks = 0;
		auto Canceled = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected), .ShouldCancel = [&] { return ++Checks >= StopAt; }, .Stats = &Stats});
		ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
		if (StopAt == 1) EXPECT_EQ(Stats.ReadBytes, 0u);
		EXPECT_LE(Stats.ValuesRead, 1u); EXPECT_LE(Stats.DecodedBytes, 1u);
		SawDecodedCancellation |= Stats.DecodedBytes == 1;
	}
	EXPECT_TRUE(SawDecodedCancellation);
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
	ASSERT_TRUE(GetCacheBackend().Put({First}));
	ASSERT_TRUE(GetCacheBackend().Put({Second}));
	uint32 Blobs = 0;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(Directory.Root / "values/v1"))
		if (Entry.is_regular_file()) ++Blobs;
	EXPECT_EQ(Blobs, 2u); // One shared nonempty blob and one explicit empty blob.
	const auto Manifest = GetCacheStorage().Get({A, 4096, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	auto Descriptor = Durin::DerivedData::Private::DecodeCacheManifest(A, **Manifest);
	ASSERT_TRUE(Descriptor);
	EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(A, *Descriptor));
	EXPECT_FALSE(Durin::DerivedData::Private::DecodeCacheManifest(B, **Manifest));
	EXPECT_FALSE(Durin::DerivedData::Private::DecodeCacheManifest(A, **Manifest, {.MaximumTotalBytes = 100}));
	EXPECT_FALSE(Descriptor->WithValues(Second.GetValues()));
	for (const auto& Record : {First, Second})
	{
		auto Loaded = GetCacheBackend().Get({Record.GetKey()});
		ASSERT_TRUE(Loaded);
		ASSERT_TRUE(*Loaded);
		auto Warm = Durin::DerivedData::Private::ReadBuildOutput(Record.GetKey(), (**Loaded));
		ASSERT_TRUE(Warm);
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
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto Blob = BlobKey(Record.GetValues()[0].Value);
	ASSERT_TRUE(GetCacheStorage().Put({Blob, Bytes({9}), 4096, ECacheStorageNamespace::Values}));
	auto Damaged = GetCacheBackend().Get({Key});
	ASSERT_FALSE(Damaged);
	EXPECT_EQ(Damaged.error().Code, ECacheError::Corrupt);
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	EXPECT_TRUE(GetCacheBackend().Get({Key}).value().has_value());
	ASSERT_TRUE(std::filesystem::remove(EntryPath(Directory, Blob, "values/v1")));
	auto Missing = GetCacheBackend().Get({Key});
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().Code, ECacheError::MissingValue);
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	EXPECT_TRUE(GetCacheBackend().Get({Key}).value().has_value());
}

TEST(FCacheTests, FailedPublicationRetainsOrphansAndExistingReaders)
{
	FScopedCacheDirectory Directory("ContentPublicationFailure");
	const auto ExistingKey = MakeKey(FCacheBucket::FromString("Existing"), 'a');
	const auto Existing = RecordWith(ExistingKey, {FSharedByteBuffer::Take(Bytes({9}))});
	ASSERT_TRUE(GetCacheBackend().Put({Existing}));
	const auto Key = MakeKey(FCacheBucket::FromString("Interrupted"), 'd');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	const auto Blocked = EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1");
	std::filesystem::create_directories(Blocked); // Fail the second blob after the first is published.
	const auto Failed = GetCacheBackend().Put({Record});
	ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.error().Code, ECacheError::StorageFailure);
	EXPECT_FALSE(GetCacheBackend().Get({Key}).value().has_value());
	EXPECT_TRUE(std::filesystem::is_regular_file(EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1")));
	EXPECT_TRUE(GetCacheBackend().Get({ExistingKey}).value().has_value());
	const auto ManifestBlockedKey = MakeKey(FCacheBucket::FromString("ManifestBlocked"), 'e');
	std::filesystem::create_directories(EntryPath(Directory, ManifestBlockedKey, "records/v5"));
	EXPECT_FALSE(GetCacheBackend().Put({RecordWith(ManifestBlockedKey, {Existing.GetValues()[0].Value.GetData()})}));
	EXPECT_TRUE(GetCacheBackend().Get({ExistingKey}).value().has_value());
}

TEST(FCacheTests, EnforcesRawEncodedAndResidentBudgetsBeforePublishingManifest)
{
	FScopedCacheDirectory Directory("ContentBudgets");
	const auto Key = MakeKey(FCacheBucket::FromString("Budgets"), 'f');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{8}))});
	EXPECT_FALSE(GetCacheBackend().Put({Record, 8192, {.MaximumTotalBytes = 100}, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCacheBackend().Put({Record, 64, {.MaximumTotalBytes = 8192}, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCacheBackend().Put({Record, 8192, {.MaximumTotalBytes = 8192}, 4096}));
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{3}));
	EXPECT_FALSE(GetCacheBackend().Put({RecordWith(Key, {Backing.MakeView(0, 1)}), 8192, {.MaximumTotalBytes = 8192}, 4096}));
	EXPECT_FALSE(GetCacheBackend().Get({Key}).value().has_value());
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto Manifest = GetCacheStorage().Get({Key, 8192, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	const auto Blob = GetCacheStorage().Get({BlobKey(Record.GetValues()[0].Value), 8192, ECacheStorageNamespace::Values}); ASSERT_TRUE(Blob); ASSERT_TRUE(*Blob);
	const auto AggregateLimit = (**Manifest).GetSize() + (**Blob).GetSize() - 1;
	EXPECT_FALSE(GetCacheBackend().Put({Record, AggregateLimit}));
	EXPECT_TRUE(GetCacheBackend().Get({Key}).value().has_value()); // Failed replacement preserves the published manifest.
	EXPECT_FALSE(GetCacheBackend().Get({Key, {}, AggregateLimit}));
	EXPECT_FALSE(GetCacheBackend().Get({Key, {.MaximumTotalBytes = 100}}));
	EXPECT_FALSE(GetCacheBackend().Get({Key, {}, 64}));
	EXPECT_FALSE(GetCacheBackend().Get({Key, {}, 8192, 4096}));
}

TEST(FCacheTests, ConcurrentContentWritersAndReadersSeeCompleteRecords)
{
	FScopedCacheDirectory Directory("ContentConcurrency");
	const auto Key = MakeKey(FCacheBucket::FromString("Concurrent"), '7');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{5}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	std::latch Start(1); std::vector<std::thread> Threads;
	for (uint32 I = 0; I < 8; ++I) Threads.emplace_back([&] {
		Start.wait();
		for (uint32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			EXPECT_TRUE(GetCacheBackend().Put({Record}));
			auto Loaded = GetCacheBackend().Get({Key});
			ASSERT_TRUE(Loaded) << Loaded.error().Diagnostic;
			ASSERT_TRUE(*Loaded);
			EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**Loaded)));
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
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key, *Output);
	ASSERT_TRUE(Record);
	ASSERT_TRUE(GetCacheBackend().Put({std::move(*Record), 4096}));

	auto Loaded = GetCacheBackend().Get({Key, {}, 4096});
	ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	auto Warm = Durin::DerivedData::Private::ReadBuildOutput(Key, (**Loaded));
	ASSERT_TRUE(Warm);
	const FValue* Value = Warm->FindValue(FValueId::FromName("Data"));
	ASSERT_NE(Value, nullptr);
	EXPECT_TRUE(std::ranges::equal(Value->GetData().GetBytes(), Bytes({1, 2, 3, 4})));
}

TEST(FCacheTests, RejectsCorruptStorageBeforeReturningARecord)
{
	FScopedCacheDirectory Directory("StructuredCacheCorruption");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Records"), '8');
	ASSERT_TRUE(GetCacheStorage().Put({Key, Bytes({9, 9, 9}), 4096, ECacheStorageNamespace::Records}));
	auto Loaded = GetCacheBackend().Get({Key, {}, 4096});
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
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key, *Output);
	ASSERT_TRUE(Record);
	auto Raw = FCacheRecordFixtureCodec::Encode(*Record, 4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecordFixtureCodec::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	const auto Path = Directory.Root / "Test" / "Objects" / "ff"
		/ (std::string(32, 'f') + ".bin");
	for (const auto& Encoded : {*Raw, *Compressed})
	{
		ASSERT_TRUE(GetCacheStorage().Put({Key, Encoded, 4096}));
		auto Stored = FFileHelper::LoadFileToArray(Path); ASSERT_TRUE(Stored);
		EXPECT_TRUE(std::ranges::equal(*Stored, Encoded.GetBytes()));
		auto Loaded = FCacheRecordFixtureCodec::Decode(Key, Encoded, {}, 4096); ASSERT_TRUE(Loaded);
		Stored->back() ^= std::byte{1};
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Stored, Path));
		auto Result = FCacheRecordFixtureCodec::Decode(Key, FSharedByteBuffer::Take(*Stored), {}, 4096);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::Corrupt);
		EXPECT_FALSE(Result.error().Diagnostic.empty());
		FByteBuffer Truncated(Encoded.begin(), Encoded.end() - 1);
		ASSERT_TRUE(GetCacheStorage().Put({Key, Truncated, 4096}));
		auto Short = FCacheRecordFixtureCodec::Decode(Key, FSharedByteBuffer::Copy(Truncated), {}, 4096); ASSERT_FALSE(Short);
		EXPECT_EQ(Short.error().Code, ECacheError::Corrupt);
		FByteBuffer Appended(Encoded.begin(), Encoded.end());
		Appended.push_back(std::byte{0});
		ASSERT_TRUE(GetCacheStorage().Put({Key, Appended, 4096}));
		auto Long = FCacheRecordFixtureCodec::Decode(Key, FSharedByteBuffer::Copy(Appended), {}, 4096); ASSERT_FALSE(Long);
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
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key, *Output);
	ASSERT_TRUE(Record);
	auto Raw = FCacheRecordFixtureCodec::Encode(*Record, 4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecordFixtureCodec::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	FBinaryWriter Legacy;
	Legacy.WriteHeader({0x43444444, 1, 1});
	Legacy.WriteU64(Compressed->GetSize());
	Legacy.WriteHash128(FXxHash128::HashBuffer(Compressed->GetBytes()));
	Legacy.WriteBytes(Compressed->GetBytes());
	ASSERT_TRUE(GetCacheStorage().Put({Key, Legacy.GetBytes(), 4096}));
	auto Rejected = GetCacheBackend().Get({Key, {}, 4096});
	ASSERT_TRUE(Rejected);
	EXPECT_FALSE(*Rejected); // Legacy namespaces are cold misses, never decoded.
	ASSERT_TRUE(GetCacheBackend().Put({*Record, 4096}));
	auto Loaded = GetCacheBackend().Get({Key, {}, 4096});
	ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key, (**Loaded)));
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

TEST(FCacheTests, RepeatedBlobIdentitiesShareReadDecodeAndPhysicalBudgets)
{
	FScopedCacheDirectory Directory("RequestLocalReuse");
	for (uint64 Size : {uint64(0), uint64(1), uint64(8192)})
	{
		SCOPED_TRACE(Size);
		const auto Key = MakeKey(FCacheBucket::FromString("Reuse"), Size ? (Size == 1 ? 'b' : 'c') : 'a');
		const auto Block = FSharedByteBuffer::Take(FByteBuffer(Size, std::byte{4}));
		const auto Record = RecordWith(Key, {Block, FSharedByteBuffer::Copy(Block.GetBytes()), Block});
		FCacheStoreStats Cold, Warm;
		ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .Stats = &Cold}));
		EXPECT_EQ(Cold.BlobsCompressed, 1u); EXPECT_EQ(Cold.BlobsWritten, 1u);
		ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .Stats = &Warm}));
		EXPECT_EQ(Warm.BlobsReused, 1u); EXPECT_EQ(Warm.BlobsCompressed, 0u); EXPECT_EQ(Warm.BlobsWritten, 0u);
		FCacheQueryStats Stats;
		auto Loaded = GetCacheBackend().Get({.Key = Key, .Stats = &Stats});
		ASSERT_TRUE(Loaded && *Loaded);
		EXPECT_EQ(Stats.ValuesRead, 1u); EXPECT_EQ(Stats.DecodedBytes, Size);
		const auto Values = (**Loaded).GetValues(); ASSERT_EQ(Values.size(), 3u);
		EXPECT_TRUE(Values[0].Value.GetData().SharesStorageWith(Values[1].Value.GetData()));
		EXPECT_TRUE(Values[0].Value.GetData().SharesStorageWith(Values[2].Value.GetData()));
		EXPECT_EQ(Values[0].Value.GetRawSize(), Size);
		EXPECT_TRUE(GetCacheBackend().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes, .MaximumWorkingSetBytes = Stats.PeakWorkingSetBytes}));
		EXPECT_FALSE(GetCacheBackend().Get({.Key = Key, .MaximumEncodedBytes = Stats.ReadBytes - 1}));
		if (Size) EXPECT_FALSE(GetCacheBackend().Get({.Key = Key, .RecordLimits = {.MaximumTotalBytes = Size * 2}}));
		const std::array Selected{Values[0].Id, Values[2].Id};
		FCacheQueryStats SelectedStats;
		auto Partial = GetCacheBackend().Get({.Key = Key, .Policy = SelectValues(Selected), .Stats = &SelectedStats});
		ASSERT_TRUE(Partial && *Partial); EXPECT_EQ(SelectedStats.ValuesRead, 1u);
		EXPECT_TRUE((**Partial).FindValue(Selected[0])->GetData().SharesStorageWith((**Partial).FindValue(Selected[1])->GetData()));
		std::cout << "[BlobReuse] raw=" << Size << " reads=" << Stats.ValuesRead
			<< " decoded=" << Stats.DecodedBytes
			<< " read_bytes=" << Stats.ReadBytes << " peak_bytes=" << Stats.PeakWorkingSetBytes
			<< " cold_codec_calls=" << Cold.BlobsCompressed << " warm_codec_calls=" << Warm.BlobsCompressed
			<< " warm_blob_writes=" << Warm.BlobsWritten << '\n';
	}
}

TEST(FCacheTests, MixedBlobIdentitiesDeduplicateOnlyMatchingContent)
{
	FScopedCacheDirectory Directory("MixedBlobIdentities");
	const auto Key = MakeKey(FCacheBucket::FromString("Reuse"), 'd');
	const auto Empty = FSharedByteBuffer::Take(FByteBuffer{});
	const auto First = FSharedByteBuffer::Take(Bytes({1, 2}));
	const auto SameSize = FSharedByteBuffer::Take(Bytes({3, 4}));
	const auto OtherSize = FSharedByteBuffer::Take(Bytes({1, 2, 3}));
	const auto Record = RecordWith(Key, {Empty, First, SameSize, OtherSize,
		FSharedByteBuffer::Copy(First.GetBytes()), Empty, SameSize});
	FCacheStoreStats Cold, Warm;
	ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .Stats = &Cold}));
	EXPECT_EQ(Cold.BlobsCompressed, 4u); EXPECT_EQ(Cold.BlobsWritten, 4u);
	ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .Stats = &Warm}));
	EXPECT_EQ(Warm.BlobsReused, 4u); EXPECT_EQ(Warm.BlobsCompressed, 0u); EXPECT_EQ(Warm.BlobsWritten, 0u);
	FCacheQueryStats Stats;
	auto Loaded = GetCacheBackend().Get({.Key = Key, .Stats = &Stats});
	ASSERT_TRUE(Loaded && *Loaded);
	EXPECT_EQ(Stats.ValuesRead, 4u); EXPECT_EQ(Stats.DecodedBytes, 7u);
	ASSERT_EQ((**Loaded).GetValues().size(), Record.GetValues().size());
	for (const auto& Item : Record.GetValues()) {
		const auto* Actual = (**Loaded).FindValue(Item.Id); ASSERT_NE(Actual, nullptr);
		EXPECT_TRUE(std::ranges::equal(Actual->GetData().GetBytes(), Item.Value.GetData().GetBytes()));
		for (const auto& Other : Record.GetValues()) {
			const bool SameContent = Durin::DerivedData::Private::BlobIdentity(Item.Value) == Durin::DerivedData::Private::BlobIdentity(Other.Value)
									 && Item.Value.GetRawSize() == Other.Value.GetRawSize();
			EXPECT_EQ(Actual->GetData().SharesStorageWith((**Loaded).FindValue(Other.Id)->GetData()), SameContent);
		}
	}
}

TEST(FCacheTests, StoredReuseVerifiesRawContentAndDoesNotRepublishValidBlobs)
{
	FScopedCacheDirectory Directory("VerifiedReuse");
	const auto Key = MakeKey(FCacheBucket::FromString("Verified"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto Path = EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1");
	const auto Stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(Path, Stamp);
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	EXPECT_EQ(std::filesystem::last_write_time(Path), Stamp);
	// Keep a valid envelope but damage raw content so envelope-only reuse would fail.
	auto Encoded = FFileHelper::LoadFileToArray(Path); ASSERT_TRUE(Encoded);
	(*Encoded)[48] ^= std::byte{1};
	FBinaryWriter Writer; Writer.WriteBytes(FByteView(*Encoded).first(Encoded->size() - 16));
	Writer.WriteHash128(FXxHash128::HashBuffer(Writer.GetBytes()));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Writer.GetBytes(), Path));
	FCacheStoreStats Stats;
	ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .Stats = &Stats}));
	EXPECT_EQ(Stats.BlobsReused, 0u); EXPECT_EQ(Stats.BlobsWritten, 1u);
	EXPECT_TRUE(GetCacheBackend().Get({Key}).value().has_value());
}

TEST(FAsyncCacheTests, SharedReadWaiterCanCancelWithoutCancelingLeader)
{
	FCacheReadCoalescer Coalescer; std::latch Entered(1), Release(1);
	std::atomic_bool Cancel = false;
	auto Leader = std::async(std::launch::async, [&] { return Coalescer.Get("cancel", [&]() -> FCacheStorageGetResult {
		Entered.count_down(); Release.wait(); return std::optional{FSharedByteBuffer::Take(Bytes({1}))};
	}); });
	Entered.wait();
	auto Waiter = std::async(std::launch::async, [&] { return Coalescer.Get("cancel", []() -> FCacheStorageGetResult {
		ADD_FAILURE() << "Waiter must join the existing physical read"; return std::optional<FSharedByteBuffer>{};
	}, nullptr, [&] { return Cancel.load(); }); });
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Coalescer.GetCoalescedReadCount() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	Cancel = true;
	const auto Ready = Waiter.wait_for(std::chrono::seconds(2));
	Release.count_down();
	EXPECT_EQ(Ready, std::future_status::ready);
	auto Canceled = Waiter.get(); ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
	EXPECT_TRUE(Leader.get().has_value());
}

TEST(FAsyncCacheTests, ReadSharingCapacityFallsBackWithoutRetainingOverflowKeys)
{
	FCacheReadCoalescer Coalescer;
	std::function<FCacheStorageGetResult(uint32)> Nested;
	Nested = [&](uint32 Depth) -> FCacheStorageGetResult {
		return Coalescer.Get(std::to_string(Depth), [&]() -> FCacheStorageGetResult {
			EXPECT_LE(Coalescer.GetPendingReadCount(), 64u);
			if (Depth == 72) { EXPECT_EQ(Coalescer.GetPendingReadCount(), 64u); return std::optional{FSharedByteBuffer::Take(Bytes({1}))}; }
			return Nested(Depth + 1);
		});
	};
	EXPECT_TRUE(Nested(0)); EXPECT_EQ(Coalescer.GetPendingReadCount(), 0u);
}

TEST(FAsyncCacheTests, CanceledReadLeaderStillPublishesSharedReadToItsWaiter)
{
	FCacheReadCoalescer Coalescer; std::latch Entered(1), Release(1); std::atomic_bool Cancel = false;
	uint64 LeaderBytes = 0;
	auto Leader = std::async(std::launch::async, [&] { return Coalescer.Get("leader", [&]() -> FCacheStorageGetResult {
		Entered.count_down(); Release.wait(); return std::optional{FSharedByteBuffer::Take(Bytes({1, 2}))};
	}, &LeaderBytes, [&] { return Cancel.load(); }); });
	Entered.wait();
	auto Waiter = std::async(std::launch::async, [&] { return Coalescer.Get("leader", []() -> FCacheStorageGetResult {
		ADD_FAILURE() << "Expected shared read"; return std::optional<FSharedByteBuffer>{};
	}); });
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Coalescer.GetCoalescedReadCount() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	EXPECT_EQ(Coalescer.GetCoalescedReadCount(), 1u); Cancel = true; Release.count_down();
	auto Canceled = Leader.get(); ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
	EXPECT_EQ(LeaderBytes, 2u); auto Result = Waiter.get(); EXPECT_TRUE(Result && *Result);
}

TEST(FCacheDiagnosticsTests, DistinguishesMissHitCorruptionBackendFailureAndCanceledStore)
{
	FScopedCacheDirectory Directory("DiagnosticOutcomes"); Durin::DerivedData::Private::ResetCacheDiagnosticsForTests();
	const auto Key = MakeKey(FCacheBucket::FromString("Diagnostics"), 'a');
	EXPECT_FALSE(GetCacheBackend().Get({Key}).value().has_value());
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	ASSERT_TRUE(GetCacheBackend().Get({Key}).value().has_value());
	ASSERT_TRUE(GetCacheStorage().Put({BlobKey(Record.GetValues()[0].Value), Bytes({9}), 100, ECacheStorageNamespace::Values}));
	EXPECT_FALSE(GetCacheBackend().Get({Key}));
	const auto ManifestPath = EntryPath(Directory, Key, "records/v5");
	ASSERT_TRUE(std::filesystem::remove(ManifestPath));
	std::filesystem::create_directory(ManifestPath);
	EXPECT_FALSE(GetCacheBackend().Get({Key}));
	EXPECT_FALSE(GetCacheBackend().Put({.Record = Record, .ShouldCancel = [] { return true; }}));
	const auto Stats = GetCacheDiagnostics();
	EXPECT_EQ(Stats.Total.Gets, 4u); EXPECT_EQ(Stats.Total.Misses, 1u); EXPECT_EQ(Stats.Total.Hits, 1u);
	EXPECT_EQ(Stats.Total.InvalidRecords, 1u); EXPECT_EQ(Stats.Total.BackendFailures, 1u);
	EXPECT_EQ(Stats.Total.Puts, 2u); EXPECT_EQ(Stats.Total.Canceled, 1u); EXPECT_EQ(Stats.Total.StoreFailures, 0u);
	EXPECT_GT(Stats.Total.ReadBytes, 0u); EXPECT_GT(Stats.Total.GetNanoseconds, 0u); EXPECT_GT(Stats.Total.PutNanoseconds, 0u);
	ASSERT_EQ(Stats.Buckets.size(), 1u); EXPECT_EQ(Stats.Buckets[0].Counters.Gets, Stats.Total.Gets);
}

TEST(FCacheDiagnosticsTests, BoundsBucketsAndObservesAdmissionWithoutChangingResults)
{
	FScopedCacheDirectory Directory("DiagnosticBounds"); Durin::DerivedData::Private::ResetCacheDiagnosticsForTests();
	for (uint32 Index = 0; Index < 70; ++Index)
		EXPECT_TRUE(GetCacheBackend().Get({MakeKey(FCacheBucket::FromString("Bucket" + std::to_string(Index)), 'a')}));
	auto Stats = GetCacheDiagnostics(); EXPECT_EQ(Stats.Buckets.size(), FCacheDiagnostics::MaximumBuckets);
	EXPECT_EQ(Stats.Total.Misses, 70u); EXPECT_EQ(Stats.OtherBuckets.Misses, 6u);
	auto Queue = std::make_shared<FCacheQueueScheduler>(); auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Async = CreateAsyncCache(Backend, Queue, {.Concurrency = 1, .Capacity = 1}); FRequestOwner Owner;
	const std::array Requests{FCacheGetBackendRequest{.Key = MakeKey(FCacheBucket::FromString("Queue"), 'b')}};
	ASSERT_TRUE(Async->Get(Requests, Owner, [](auto, auto, auto) {}));
	auto Rejected = Async->Get(Requests, Owner, [](auto, auto, auto) { ADD_FAILURE(); }); ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Code, ECacheError::Capacity); Queue->Run();
	Stats = GetCacheDiagnostics(); EXPECT_EQ(Stats.Total.CapacityRejections, 1u); EXPECT_EQ(Stats.Total.AsyncStarts, 1u);
	EXPECT_GT(Stats.Total.QueueNanoseconds, 0u); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FCacheStorageTests, RecordTransactionsKeepTheirLeasedRootAcrossAllFiles)
{
	FScopedCacheDirectory First("TransactionRootFirst");
	FScopedCacheDirectory Second("TransactionRootSecond");
	const auto Key = MakeKey(FCacheBucket::FromString("Ownership"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2})), FSharedByteBuffer::Take(Bytes({3, 4}))});
	FPaths::SetDerivedDataCacheDirForTests(First.Root.generic_string());
	uint32 Checks = 0;
	// Switch only through a synchronous test callback, after transaction admission.
	// Production root changes still require drained work.
	auto SwitchRoot = [&] {
		if (++Checks == 2) {
			FPaths::SetDerivedDataCacheDirForTests(Second.Root.generic_string());
			EXPECT_FALSE(MaintainCacheStorage({.Root = First.Root}));
		}
		return false;
	};
	ASSERT_TRUE(GetCacheBackend().Put({.Record = Record, .ShouldCancel = SwitchRoot}));
	EXPECT_GE(Checks, 2u);
	auto Other = GetCacheBackend().Get({Key});
	ASSERT_TRUE(Other);
	EXPECT_FALSE(*Other);
	// The completed transaction released its request lease; changing roots also
	// replaced the process lease, so maintenance can now acquire the first root.
	EXPECT_TRUE(MaintainCacheStorage({.Root = First.Root}));
	FPaths::SetDerivedDataCacheDirForTests(First.Root.generic_string()); Checks = 0;
	auto Loaded = GetCacheBackend().Get({.Key = Key, .ShouldCancel = SwitchRoot});
	ASSERT_TRUE(Loaded); ASSERT_TRUE(*Loaded); EXPECT_GE(Checks, 2u);
	ASSERT_EQ((**Loaded).GetValues().size(), 2u);
	for (const auto& Value : Record.GetValues()) {
		const auto* Actual = (**Loaded).FindValue(Value.Id); ASSERT_NE(Actual, nullptr);
		EXPECT_TRUE(std::ranges::equal(Actual->GetData().GetBytes(), Value.Value.GetData().GetBytes()));
	}
	Other = GetCacheBackend().Get({Key});
	ASSERT_TRUE(Other);
	EXPECT_FALSE(*Other);
	EXPECT_TRUE(MaintainCacheStorage({.Root = First.Root}));
}

TEST(FCacheMaintenanceTests, RefusesLiveRootAndNormalAccessRefusesExclusiveMaintenance)
{
	FScopedCacheDirectory Directory("OfflineOwnership");
	const auto Key = MakeKey(FCacheBucket::FromString("Ownership"), 'a');
	ASSERT_TRUE(GetCacheBackend().Get({Key}));
	for (auto Operation : {ECacheMaintenanceOperation::Inspect, ECacheMaintenanceOperation::Verify, ECacheMaintenanceOperation::Clear})
		EXPECT_FALSE(MaintainCacheStorage({.Root = Directory.Root, .Operation = Operation}));
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	auto Lease = Durin::DerivedData::Private::OpenCacheStorageLease(Directory.Root, true); ASSERT_TRUE(Lease);
	EXPECT_FALSE(Durin::DerivedData::Private::OpenCacheStorageLease(Directory.Root, true));
	EXPECT_FALSE(GetCacheBackend().Get({Key}));
	EXPECT_FALSE(GetCacheBackend().Put({RecordWith(Key, {})}));
	uint64 ReadBytes = 99;
	EXPECT_FALSE(GetCacheStorage().Get({Key, 1024, ECacheStorageNamespace::Records, &ReadBytes}));
	EXPECT_EQ(ReadBytes, 0u);
	Lease->reset();
	EXPECT_TRUE(GetCacheBackend().Get({Key}));
}

TEST(FCacheMaintenanceTests, InspectsVerifiesAndClearsOnlyRecognizedStorage)
{
	FScopedCacheDirectory Directory("OfflineRoundTrip"); const auto Key = MakeKey(FCacheBucket::FromString("Nested/Bucket"), 'b');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{3})), {}});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	const auto Other = Directory.Root / "unrelated.txt"; ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({9}), Other));
	auto Inspection = MaintainCacheStorage({.Root = Directory.Root}); ASSERT_TRUE(Inspection);
	EXPECT_EQ(Inspection->RecordFiles, 1u); EXPECT_EQ(Inspection->ValueFiles, 2u); EXPECT_EQ(Inspection->ReadBytes, 0u);
	EXPECT_EQ(Inspection->OtherRootEntries, 1u);
	auto Verified = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify});
	ASSERT_TRUE(Verified); EXPECT_TRUE(Verified->IsValid()); EXPECT_EQ(Verified->VerifiedFiles, 3u);
	EXPECT_GT(Verified->ReadBytes, 0u);
	auto Cleared = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Clear});
	ASSERT_TRUE(Cleared); EXPECT_EQ(Cleared->RemovedFiles, 3u);
	EXPECT_EQ(Cleared->RemovedBytes, Inspection->RecordBytes + Inspection->ValueBytes);
	EXPECT_TRUE(std::filesystem::is_regular_file(Other)); EXPECT_TRUE(std::filesystem::is_regular_file(Directory.Root / Durin::DerivedData::Private::CacheLockName));
	EXPECT_TRUE(MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Clear}));
	EXPECT_FALSE(GetCacheBackend().Get({Key}).value().has_value());
}

TEST(FCacheMaintenanceTests, ReportsCorruptRawContentMissingValuesAndUnsupportedNamespaces)
{
	FScopedCacheDirectory Directory("OfflineValidation"); const auto Key = MakeKey(FCacheBucket::FromString("Integrity"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto BlobPath = EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1");
	auto BytesOnDisk = FFileHelper::LoadFileToArray(BlobPath).value(); BytesOnDisk[48] ^= std::byte{1};
	FBinaryWriter Writer; Writer.WriteBytes(FByteView(BytesOnDisk).first(BytesOnDisk.size() - 16)); Writer.WriteHash128(FXxHash128::HashBuffer(Writer.GetBytes()));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Writer.GetBytes(), BlobPath)); Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	auto Corrupt = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify});
	ASSERT_TRUE(Corrupt); EXPECT_FALSE(Corrupt->IsValid()); EXPECT_GT(Corrupt->InvalidFiles, 0u);
	ASSERT_TRUE(std::filesystem::remove(BlobPath));
	auto Missing = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify});
	ASSERT_TRUE(Missing); EXPECT_EQ(Missing->MissingValues, 1u); EXPECT_FALSE(Missing->IsValid());
	const auto Legacy = Directory.Root / "records/v3/old.bin"; std::filesystem::create_directories(Legacy.parent_path());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({1}), Legacy));
	auto Unsupported = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify});
	ASSERT_TRUE(Unsupported); EXPECT_EQ(Unsupported->UnsupportedFiles, 1u); EXPECT_TRUE(std::filesystem::exists(Legacy));
}

TEST(FCacheMaintenanceTests, LimitsAndCancellationNeverReportPartialVerificationAsSuccess)
{
	FScopedCacheDirectory Directory("OfflineLimits"); const auto Key = MakeKey(FCacheBucket::FromString("Limits"), 'd');
	ASSERT_TRUE(GetCacheBackend().Put({RecordWith(Key, {FSharedByteBuffer::Take(Bytes({4}))})}));
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	for (auto Operation : {ECacheMaintenanceOperation::Inspect, ECacheMaintenanceOperation::Verify, ECacheMaintenanceOperation::Clear})
	{
		auto Limited = MaintainCacheStorage({.Root = Directory.Root, .Operation = Operation, .MaximumEntries = 1});
		ASSERT_FALSE(Limited); EXPECT_EQ(Limited.error().Code, ECacheError::ValueTooLarge);
		auto Canceled = MaintainCacheStorage({.Root = Directory.Root, .Operation = Operation, .ShouldCancel = [] { return true; }});
		ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
	}
	for (auto Options : {FCacheMaintenanceOptions{.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify, .MaximumFileBytes = 1},
		FCacheMaintenanceOptions{.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify, .MaximumReadBytes = 1},
		FCacheMaintenanceOptions{.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Verify, .MaximumWorkingSetBytes = 1}})
	{ auto Limited = MaintainCacheStorage(Options); ASSERT_FALSE(Limited); EXPECT_EQ(Limited.error().Code, ECacheError::ValueTooLarge); }
	EXPECT_TRUE(GetCacheBackend().Get({Key}).value().has_value());
}

TEST(FCacheMaintenanceTests, RefusesLinksBeforeDeletingAnyContent)
{
	FScopedCacheDirectory Directory("OfflineLinks"); const auto Key = MakeKey(FCacheBucket::FromString("Links"), 'e');
	ASSERT_TRUE(GetCacheBackend().Put({RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1}))})}));
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	const auto Outside = Testing::GetTestWorkDirectory() / "OutsideCache.txt"; ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes({9}), Outside));
	std::error_code Ec; std::filesystem::create_symlink(Outside, Directory.Root / "values/link.bin", Ec);
	if (Ec) GTEST_SKIP() << "Symlink creation unavailable: " << Ec.message();
	for (auto Operation : {ECacheMaintenanceOperation::Inspect, ECacheMaintenanceOperation::Verify, ECacheMaintenanceOperation::Clear})
		EXPECT_FALSE(MaintainCacheStorage({.Root = Directory.Root, .Operation = Operation}));
	EXPECT_TRUE(std::filesystem::exists(EntryPath(Directory, Key, "records/v5")));
	EXPECT_TRUE(std::filesystem::exists(Outside));
}

TEST(FCacheMaintenanceDeathTests, AnotherProcessCannotMaintainLiveStorageAndCrashReleasesOwnership)
{
	FScopedCacheDirectory Directory("OfflineProcesses"); const auto Key = MakeKey(FCacheBucket::FromString("Processes"), 'a');
	ASSERT_TRUE(GetCacheBackend().Get({Key}));
	EXPECT_EXIT({ auto Lease = Durin::DerivedData::Private::OpenCacheStorageLease(Directory.Root, true); std::_Exit(Lease ? 1 : 0); }, testing::ExitedWithCode(0), "");
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	EXPECT_EXIT({ auto Lease = Durin::DerivedData::Private::OpenCacheStorageLease(Directory.Root, false); std::_Exit(Lease ? 0 : 1); }, testing::ExitedWithCode(0), "");
	EXPECT_TRUE(Durin::DerivedData::Private::OpenCacheStorageLease(Directory.Root, true));
}

TEST(FCacheMaintenanceTests, CanceledClearLeavesValuesUntilManifestsAreRemovedAndCanResume)
{
	FScopedCacheDirectory Directory("OfflineClearResume"); const auto Key = MakeKey(FCacheBucket::FromString("Resume"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	Durin::DerivedData::Private::ReleaseCacheStorageOwnership();
	const auto Manifest = EntryPath(Directory, Key, "records/v5");
	const auto Blob = EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1");
	auto Canceled = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Clear,
		.ShouldCancel = [&] { return !std::filesystem::exists(Manifest); }});
	ASSERT_FALSE(Canceled); EXPECT_EQ(Canceled.error().Code, ECacheError::Canceled);
	EXPECT_FALSE(std::filesystem::exists(Manifest)); EXPECT_TRUE(std::filesystem::exists(Blob));
	auto Resumed = MaintainCacheStorage({.Root = Directory.Root, .Operation = ECacheMaintenanceOperation::Clear});
	ASSERT_TRUE(Resumed); EXPECT_FALSE(std::filesystem::exists(Blob));
	EXPECT_FALSE(GetCacheBackend().Get({Key}).value().has_value());
}

TEST(FPublicCacheTests, BackendResourceLimitsApplyWithoutRequestStorageOptions)
{
	FScopedCacheDirectory Directory("PublicCacheResourceLimits");
	const auto Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'd');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	FCacheResourceLimits Raw, Metadata, Values, Encoded, Workspace;
	Raw.Record.MaximumTotalBytes = 1;
	Metadata.Record.MaximumMetadataBytes = 0;
	Values.Record.MaximumValues = 0;
	Encoded.MaximumEncodedBytes = 1;
	Workspace.MaximumWorkingSetBytes = 1;
	const std::array Gets{FCacheGetRequest{.Key = Key}};
	const std::array Puts{FCachePutRequest{.Record = Record}};
	for (const auto& Limits : {Raw, Metadata, Values, Encoded, Workspace})
	{
		auto Cache = Durin::DerivedData::Private::CreateCacheForBackend({}, {}, {}, Limits);
		FRequestOwner Owner;
		uint32 Calls = 0;
		Cache->Get(Gets, Owner, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Error); ++Calls; });
		Cache->Put(Puts, Owner, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Error); ++Calls; });
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(Calls, 2u);
		auto Retained = GetCacheBackend().Get({Key});
		ASSERT_TRUE(Retained && *Retained);
		EXPECT_EQ((**Retained).GetValues()[0].Value.GetRawHash(), Record.GetValues()[0].Value.GetRawHash());
	}
}

TEST(FPublicCacheTests, PolicyOutlivesCallerStorageAndSelectsResidentData)
{
	FScopedCacheDirectory Directory("PublicCachePolicyLifetime");
	const auto Key = MakeKey(FCacheBucket::FromString("PublicPolicy"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	ASSERT_TRUE(GetCacheBackend().Put({Record}));
	const auto Selected = Record.GetValues()[0].Id, Skipped = Record.GetValues()[1].Id;
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend({}, CreateAsyncCache({}, Scheduler));
	FRequestOwner Owner;
	uint32 Calls = 0;
	{
		FCacheRecordPolicyBuilder Builder(ECachePolicy::None);
		Builder.AddValuePolicy(Selected, ECachePolicy::Default);
		const std::array Requests{FCacheGetRequest{.Key = Key, .Policy = Builder.Build()}};
		Cache->Get(Requests, Owner, [&](auto Response) {
			++Calls;
			EXPECT_EQ(Response.Status, EStatus::Ok);
			ASSERT_NE(Response.Record.FindValue(Selected), nullptr);
			ASSERT_NE(Response.Record.FindValue(Skipped), nullptr);
			EXPECT_TRUE(Response.Record.FindValue(Selected)->HasData());
			EXPECT_FALSE(Response.Record.FindValue(Skipped)->HasData());
			EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		});
	}
	EXPECT_FALSE(Owner.Poll());
	EXPECT_EQ(Calls, 0u);
	ASSERT_EQ(Scheduler->Work.size(), 1u);
	Scheduler->Run();
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
}

TEST(FPublicCacheTests, ResponsesCarryIdentityAndNestedCallbacksStayOwned)
{
	FScopedCacheDirectory Directory("PublicCacheResponses");
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend();
	FRequestOwner Owner;
	const auto Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'a');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	uint32 Calls = 0;
	const std::array Puts{FCachePutRequest{.Record = Record, .Name = "put", .UserData = 41}};
	Cache->Put(Puts, Owner, [&](FCachePutResponse Response) {
		++Calls;
		EXPECT_EQ(Response.Status, EStatus::Ok);
		EXPECT_EQ(Response.Key, Key);
		EXPECT_EQ(Response.Name, "put");
		EXPECT_EQ(Response.UserData, 41u);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		const std::array Gets{FCacheGetRequest{.Key = Key, .Name = "get", .UserData = 42}};
		Cache->Get(Gets, Owner, [&](FCacheGetResponse Value) {
			++Calls;
			EXPECT_EQ(Value.Status, EStatus::Ok);
			EXPECT_TRUE(Value.Record.IsValid());
			EXPECT_EQ(Value.Record.GetKey(), Key);
			EXPECT_EQ(Value.Name, "get");
			EXPECT_EQ(Value.UserData, 42u);
			EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		});
	});
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Calls, 2u);
}

TEST(FPublicCacheTests, RejectedBatchCompletesEveryItemAndQueuedCancellationCompletesOnce)
{
	auto Scheduler = std::make_shared<FCacheQueueScheduler>();
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend(Backend, CreateAsyncCache(Backend, Scheduler, {.Concurrency = 1, .Capacity = 1}));
	FRequestOwner Pending, Rejected;
	const auto Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'b');
	const std::array Requests{FCacheGetRequest{.Key = Key, .UserData = 17}};
	uint32 Canceled = 0, Failed = 0;
	Cache->Get(Requests, Pending, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Canceled); ++Canceled; });
	std::vector Overflow{Requests[0], FCacheGetRequest{.Key = Key, .UserData = 18}};
	Cache->Get(Overflow, Rejected, [&](auto Response) {
		EXPECT_EQ(Response.Status, EStatus::Error);
		EXPECT_FALSE(Response.Record.IsValid());
		EXPECT_EQ(Response.UserData, 17u + Failed);
		++Failed;
		if (Failed == 1) std::vector<FCacheGetRequest>().swap(Overflow);
	});
	EXPECT_EQ(Failed, 2u);
	EXPECT_TRUE(Rejected.Poll());
	EXPECT_FALSE(Pending.Poll());
	Pending.Cancel();
	EXPECT_TRUE(Pending.Poll());
	EXPECT_EQ(Canceled, 1u);
	Scheduler->Run();
	EXPECT_EQ(Canceled, 1u);
	EXPECT_EQ(Backend->Gets, 0u);
}

TEST(FPublicCacheTests, BackendFailureAndAlreadyCanceledOwnerUseTerminalResponses)
{
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	Backend->Throw = true;
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend(Backend);
	const std::array Requests{FCacheGetRequest{.Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'c')}};
	FRequestOwner Failed, Canceled;
	uint32 Calls = 0;
	Cache->Get(Requests, Failed, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Error); ++Calls; });
	Canceled.Cancel();
	Cache->Get(Requests, Canceled, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Canceled); ++Calls; });
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Backend->Gets, 1u);
	EXPECT_TRUE(Failed.Poll());
	EXPECT_TRUE(Canceled.Poll());
}

TEST(FPublicCacheTests, AcceptedBatchSharesCallbackStateAndRetainsRequestTags)
{
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend(Backend);
	const auto Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'd');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({3, 4}))});
	std::vector Gets{FCacheGetRequest{.Key = Key, .Name = "first", .UserData = 1}, FCacheGetRequest{.Key = Key, .Name = "second", .UserData = 2}};
	std::vector Puts{FCachePutRequest{.Record = Record, .Name = "first", .UserData = 1}, FCachePutRequest{.Record = Record, .Name = "second", .UserData = 2}};
	FRequestOwner Owner;
	std::vector<uint32> GetOrder, PutOrder;
	Cache->Get(Gets, Owner, [&, Count = 0u](auto Response) mutable {
		GetOrder.push_back(++Count);
		EXPECT_EQ(Response.UserData, Count);
		EXPECT_EQ(Response.Name, Count == 1 ? "first" : "second");
		EXPECT_EQ(Response.Status, EStatus::Error);
		if (Count == 1) std::vector<FCacheGetRequest>().swap(Gets);
	});
	Cache->Put(Puts, Owner, [&, Count = 0u](auto Response) mutable {
		PutOrder.push_back(++Count);
		EXPECT_EQ(Response.UserData, Count);
		EXPECT_EQ(Response.Name, Count == 1 ? "first" : "second");
		EXPECT_EQ(Response.Status, EStatus::Ok);
		if (Count == 1) std::vector<FCachePutRequest>().swap(Puts);
	});
	EXPECT_EQ(GetOrder, (std::vector<uint32>{1, 2}));
	EXPECT_EQ(PutOrder, (std::vector<uint32>{1, 2}));
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FPublicCacheTests, RejectedPutBatchRetainsTagsWhenCallbackReleasesRequests)
{
	auto Backend = std::make_shared<FAsyncFixtureCache>();
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend(Backend, CreateAsyncCache(Backend, CreateInlineBuildScheduler(), {.Capacity = 1}));
	const auto Key = MakeKey(FCacheBucket::FromString("PublicCache"), 'e');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({5}))});
	std::vector Requests{FCachePutRequest{.Record = Record, .UserData = 1}, FCachePutRequest{.Record = Record, .UserData = 2}};
	FRequestOwner Owner;
	uint32 Calls = 0;
	Cache->Put(Requests, Owner, [&](auto Response) {
		++Calls;
		EXPECT_EQ(Response.Status, EStatus::Error);
		EXPECT_EQ(Response.Key, Key);
		EXPECT_EQ(Response.UserData, Calls);
		if (Calls == 1) std::vector<FCachePutRequest>().swap(Requests);
	});
	EXPECT_EQ(Calls, 2u);
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Backend->Puts, 0u);
}

TEST(FGenericCacheRecordTests, StorageRoundTripsOpaqueMetadataWithoutBuildSchema)
{
	FScopedCacheDirectory Directory("OpaqueCacheRecord");
	const auto Key = MakeKey(FCacheBucket::FromString("Opaque"), 'f');
	FCbWriter Meta;
	Meta.AddString("arbitrary producer data", "Purpose");
	Meta.AddUInt64(7, "UserVersion");
	FCacheRecordBuilder Builder(Key);
	ASSERT_TRUE(Builder.SetMeta(Meta.SaveObject().value()));
	const FValue Value(FSharedByteBuffer::Take(Bytes({4, 2})));
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("OpaqueValue"), Value));
	auto Record = std::move(Builder).Build();
	ASSERT_TRUE(Record);
	ASSERT_TRUE(GetCacheBackend().Put({*Record}));
	auto Loaded = GetCacheBackend().Get({.Key = Key});
	ASSERT_TRUE(Loaded && *Loaded);
	EXPECT_EQ((**Loaded).GetMeta().GetView().Find("Purpose").AsString(), "arbitrary producer data");
	EXPECT_EQ((**Loaded).GetMeta().GetView().Find("UserVersion").AsUInt64(), 7u);
	ASSERT_EQ((**Loaded).GetValues().size(), 1u);
	EXPECT_EQ((**Loaded).GetValues()[0].Value.GetRawHash(), Value.GetRawHash());
	EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key, **Loaded));
}

TEST(FGenericCacheRecordTests, BuilderValidatesGenericValuesAndRetainsMetadataWhenRemovingData)
{
	const auto Key = MakeKey(FCacheBucket::FromString("Opaque"), 'a');
	FCbWriter Meta;
	Meta.AddUInt64(42, "Meaning");
	const auto Object = Meta.SaveObject().value();
	FCacheRecordBuilder Builder(Key);
	ASSERT_TRUE(Builder.SetMeta(Object));
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("A"), FValue(FSharedByteBuffer::Take(Bytes({1})))));
	auto Record = std::move(Builder).Build();
	ASSERT_TRUE(Record);
	auto Reference = Record->RemoveValueData();
	EXPECT_FALSE(Reference.GetValues()[0].Value.HasData());
	EXPECT_TRUE(Reference.GetMeta().GetBytes().SharesStorageWith(Object.GetBytes()));
	EXPECT_FALSE(Reference.CheckLimits({}, true));
	EXPECT_TRUE(Reference.CheckLimits());
	FCacheRecordBuilder Duplicate(Key);
	Duplicate.AddValue(FValueId::FromName("A"), Record->GetValues()[0].Value);
	Duplicate.AddValue(FValueId::FromName("A"), Record->GetValues()[0].Value);
	EXPECT_FALSE(std::move(Duplicate).Build());
	EXPECT_FALSE(std::move(FCacheRecordBuilder(FCacheKey{})).Build());
}

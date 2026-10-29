#include "BuildServiceBackendPrivate.h"
#include "BuildCacheRecordPrivate.h"
#include "RequestOwnerInternal.h"
#include "CacheBackend.h"
#include "DerivedDataBuildSession.h"
#include "CacheDiagnosticsPrivate.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskComposition.h"
#include "CacheDispatchPrivate.h"
#include <gtest/gtest.h>
#include <barrier>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	struct FFunction final : IBuildFunction
	{
		mutable std::atomic<uint32> Builds = 0;
		std::function<void()> OnBuild;
		bool FailExecution = false, Negative = false;
		uint32 DataBytes = 32;
		auto GetName() const -> std::string_view override { return "Session.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& Context) const -> void override
		{ Context.SetConstantsSchema(1); Context.SetOutput("Fixture.Output", 1); Context.SetCacheBucket(FCacheBucket::FromString("Sessions")); }
		auto Build(FBuildContext& Context) const -> void override
		{
			++Builds; if (OnBuild) OnBuild();
			if (FailExecution) return Context.FailExecution("temporary execution failure");
			if (Negative) { Context.AddError("deterministic fixture failure"); return; }
			Context.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(FByteBuffer(DataBytes, std::byte{7})));
		}
	};
	struct FSecondFunction final : IBuildFunction
	{
		auto GetName() const -> std::string_view override { return "Session.Second"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& Context) const -> void override
		{ Context.SetConstantsSchema(1); Context.SetOutput("Fixture.Output", 1); Context.SetCacheBucket(FCacheBucket::FromString("Sessions")); }
		auto Build(FBuildContext& Context) const -> void override
		{ Context.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(FByteBuffer(1, std::byte{1}))); }
	};
	auto Definition(std::string Name = "Session.Fixture") -> FBuildDefinition
	{ return std::move(FBuildDefinitionBuilder(std::move(Name))).Build().value(); }
	auto Options() -> FBuildRequestOptions
	{ return {.Policy = EBuildPolicy::Build}; }
	using FDispatcher = std::function<std::expected<void, FBuildAdmissionError>(std::function<void()>)>;
	class FTestScheduler final : public IBuildScheduler
	{
	public:
		FDispatcher Dispatcher;
		explicit FTestScheduler(FDispatcher Value) : Dispatcher(std::move(Value)) {}
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			auto Result = Dispatcher(std::move(Work));
			if (!Result) return std::unexpected(std::move(Result.error()));
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	struct FFixture
	{
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		std::shared_ptr<IBuild> Service = Durin::DerivedData::Private::CreateBuildForBackend();
		FFixture() { Service->Register(Function).value(); }
		auto Session(FDispatcher Dispatcher = {}) -> std::shared_ptr<FBuildSession>
		{ return Service->CreateSession({}, Dispatcher ? std::make_shared<FTestScheduler>(std::move(Dispatcher)) : nullptr).value(); }
	};
}

namespace
{
	class FStageScheduler final : public IBuildScheduler
	{
	public:
		std::deque<std::function<void()>> Work;
		std::vector<ERequestPriority> Priorities;
		bool Reject = false;
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Task)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			if (Reject) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, "fixture reject"});
			Priorities.push_back(Params.Priority);
			Work.push_back(std::move(Task)); return std::shared_ptr<IBuildScheduledWork>{};
		}
		auto Run() -> void { auto Task = std::move(Work.front()); Work.pop_front(); Task(); }
	};
	class FContinuationCache final : public ICacheBackend
	{
	public:
		mutable std::atomic<uint32> Gets = 0, Puts = 0;
		bool Hit = false, Partial = false, ThrowPut = false, FailPut = true;
		std::string OutputSchema = "Fixture.Output";
		uint32 OutputVersion = 1;
		bool Negative = false;
		std::function<void()> BeforeGet, BeforePut;
		auto Get(const FCacheGetBackendRequest& Request) const -> FCacheGetResult override
		{
			++Gets; if (BeforeGet) BeforeGet(); if (!Hit) return std::optional<FCacheRecord>{};
			FBuildOutputBuilder Builder(OutputSchema, OutputVersion);
			if (Negative) Builder.AddMessage(EBuildMessageSeverity::Error, "deterministic fixture failure");
			else Builder.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(FByteBuffer(32, std::byte{7})));
			auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Request.Key, std::move(Builder).Build().value()).value();
			return std::optional{Partial ? Record.RemoveValueData() : Record};
		}
		auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult override
		{
			++Puts;
			if (BeforePut) BeforePut();
			EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Request.Record.GetKey(), Request.Record));
			if (ThrowPut) throw std::runtime_error("fixture persistence exception");
			if (FailPut) return std::unexpected(FCacheError{ECacheError::StorageFailure, "fixture failed persistence"});
			return {};
		}
	};
}

TEST(FBuildContinuationTests, OwnerWaitIncludesTrackedNativeTaskEnd)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FThreadEvent Entered, Release;
	FRequestOwner Owner;
	auto Task = Tasks::LaunchTask("OwnerTrackedTask", [&] { Entered.Trigger(); Release.Wait(); });
	Durin::DerivedData::Private::FOwnerAccess::TrackTask(Owner, Task);
	Entered.Wait();
	EXPECT_FALSE(Owner.Poll());
	Release.Trigger(); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(Task.IsCompleted()); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, OwnerWaitIncludesDependencyAddedByEarlierWait)
{
	FThreadEvent Entered, Release, Finished;
	std::atomic_bool Added = false, Ready = false;
	// Owner destruction can invoke dependency cancellation after Wait completes.
	// Keep every borrowed event/flag alive until that boundary.
	FRequestOwner Owner;
	Durin::DerivedData::Private::FOwnerAccess::TrackDependency(Owner, {.IsFinished = [&] { return Added.load(); }, .Wait = [&] {
		if (!Added.exchange(true)) Durin::DerivedData::Private::FOwnerAccess::TrackDependency(Owner, {.IsFinished = [&] { return Ready.load(); },
			.Wait = [&] { Entered.Trigger(); Release.Wait(); Ready = true; return ERequestWaitResult::Completed; }, .RequestCancel = [&] { Release.Trigger(); }});
		return ERequestWaitResult::Completed; }, .RequestCancel = [] {}});
	std::thread Waiter([&] { EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); Finished.Trigger(); });
	Entered.Wait(); EXPECT_FALSE(Owner.Poll()); EXPECT_FALSE(Finished.WaitFor(0.02));
	Release.Trigger(); Waiter.join(); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, OwnerDependencyQueriesAndLateCancellationRunOutsideLock)
{
	FRequestOwner Owner; uint32 Cancellations = 0;
	Owner.RequestCancel();
	Durin::DerivedData::Private::FOwnerAccess::TrackDependency(Owner, {.IsFinished = [&] { EXPECT_EQ(Owner.GetPriority(), ERequestPriority::Normal); return true; }, .Wait = [&] { EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock); return ERequestWaitResult::Completed; }, .RequestCancel = [&] { EXPECT_TRUE(Owner.IsCanceled()); ++Cancellations; }});
	EXPECT_EQ(Cancellations, 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, SubmissionBarriersRetainNestedInlineCallbacksAndPriority)
{
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend();
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, CreateInlineBuildScheduler()).value();
	FRequestOwner Owner(ERequestPriority::Low); uint32 Callbacks = 0;
	{
		FRequestBarrier Submission(Owner);
		EXPECT_FALSE(Owner.Poll()); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		Owner.SetPriority(ERequestPriority::High);
		Function->OnBuild = [&] { EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High); };
		Session->Build(Definition(), Owner, [&](auto Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Callbacks;
			FRequestBarrier Nested(Owner);
			Session->Build(Definition(), Owner, [&](auto Other) {
				EXPECT_EQ(Other.GetStatus(), EStatus::Ok); ++Callbacks;
				EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
			}, {}, Options()); }, {}, Options());
		EXPECT_EQ(Callbacks, 2u);
		EXPECT_FALSE(Owner.Poll());
	}
	EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, CancellationCannotRetireUnpublishedSubmission)
{
	FRequestOwner Owner; FThreadEvent Entered, Release, Waiting, Retired;
	std::thread Submitter([&] { FRequestBarrier Submission(Owner); Entered.Trigger(); Release.Wait(); });
	Entered.Wait(); Owner.RequestCancel(); EXPECT_FALSE(Owner.Poll());
	std::thread Waiter([&] { Waiting.Trigger(); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); Retired.Trigger(); });
	Waiting.Wait(); EXPECT_FALSE(Retired.WaitFor(0.02));
	Release.Trigger(); Submitter.join(); Waiter.join(); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, TaskSessionsDoNotChangeInlineSessions)
{
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto ExistingInline = Service->CreateSession({}, CreateInlineBuildScheduler()).value();
	auto TaskSession = Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	auto LaterInline = Service->CreateSession({}, CreateInlineBuildScheduler()).value();
	const auto Caller = std::this_thread::get_id(); uint32 Callbacks = 0;
	Function->OnBuild = [&] { EXPECT_EQ(std::this_thread::get_id(), Caller); };
	for (const auto& Session : {ExistingInline, LaterInline})
	{
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](auto Completion) {
			EXPECT_EQ(std::this_thread::get_id(), Caller);
			EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
			EXPECT_TRUE(Completion.GetOutput());
			++Callbacks;
		});
		EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
	}
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Cache->Gets, 2u); EXPECT_EQ(Cache->Puts, 2u);
	EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, ServiceTaskSessionsShareAdmissionPriorityAndKeepInlineIndependent)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FThreadEvent Entered, Release;
	std::atomic<bool> BlockFirst = true;
	std::atomic<uint32> Calls = 0;
	std::mutex OrderMutex; std::vector<ERequestPriority> Order;
	auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	Cache->BeforeGet = [&] { if (BlockFirst.exchange(false)) { Entered.Trigger(); Release.Wait(); } };
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .CacheDispatchLimits = {.Concurrency = 1, .Capacity = 3}});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value();
	auto Second = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value();
	FRequestOwner Low(ERequestPriority::Low), High(ERequestPriority::High), Rejected;
	const auto Complete = [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; };
	const auto RecordLow = [&](auto Result) { Complete(std::move(Result)); std::lock_guard Lock(OrderMutex); Order.push_back(ERequestPriority::Low); };
	const auto RecordHigh = [&](auto Result) { Complete(std::move(Result)); std::lock_guard Lock(OrderMutex); Order.push_back(ERequestPriority::High); };
	struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } ReleaseOnExit{Release};
	First->Build(Definition(), Low, RecordLow);
	ASSERT_TRUE(Entered.WaitFor(5.0));
	First->Build(Definition(), Low, RecordLow);
	Second->Build(Definition(), High, RecordHigh);
	EXPECT_EQ(Cache->Gets, 1u);
	Second->Build(Definition(), Rejected, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Error);
		EXPECT_FALSE(Result.GetOutput());
		++Calls;
	},
				  {}, {.Policy = EBuildPolicy::Cache});
	EXPECT_TRUE(Rejected.Poll());
	EXPECT_TRUE(Rejected.Poll());
	// Task capacity does not force inline callers to help queued asset work.
	auto Inline = Service->CreateSession().value();
	FRequestOwner InlineRequestOwner;
	Inline->Build(Definition(), InlineRequestOwner, Complete);
	EXPECT_TRUE(InlineRequestOwner.Poll());
	EXPECT_EQ(Cache->Gets, 2u); EXPECT_EQ(Calls.load(), 2u);
	Release.Trigger();
	EXPECT_EQ(Low.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(High.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Order, (std::vector{ERequestPriority::Low, ERequestPriority::High, ERequestPriority::Low}));
	EXPECT_EQ(Calls.load(), 5u); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, SessionCloseDoesNotCancelOtherSessionsAndServiceCloseRetiresAll)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO, {.Concurrency = 1})});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value();
	auto Second = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value();
	uint32 Canceled = 0, Completed = 0;
	FRequestOwner RequestOwner3;
	First->Build(Definition(), RequestOwner3, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); ++Canceled; });
	FRequestOwner RequestOwner2;
	Second->Build(Definition(), RequestOwner2, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Completed; });
	First->Close(); EXPECT_EQ(Canceled, 1u);
	while (!IO->Work.empty()) IO->Run();
	EXPECT_EQ(Completed, 1u); EXPECT_EQ(Cache->Gets, 1u);
	FRequestOwner RequestOwner1;
	Second->Build(Definition(), RequestOwner1, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); ++Canceled; });
	Service->Close(); while (!IO->Work.empty()) IO->Run();
	EXPECT_EQ(Canceled, 2u); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, ServiceRetainedByteAdmissionPreservesLocalOutputAndRecoversAfterRetirement)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO, {.Concurrency = 1, .MaximumRetainedBytes = 14000})});
	auto Function = std::make_shared<FFunction>(); Function->DataBytes = 8192; ASSERT_TRUE(Service->Register(Function));
	auto First = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value();
	auto Second = Service->CreateSession({}, {}, EBuildCacheExecution::Task).value(); uint32 Calls = 0;
	const auto Complete = [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ASSERT_TRUE(Result.GetOutput()); ++Calls; };
	FBuildRequestOptions Options; Options.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore;
	FRequestOwner RequestOwner3;
	First->Build(Definition(), RequestOwner3, Complete, {}, Options);
	EXPECT_EQ(IO->Work.size(), 1u);
	FRequestOwner OverflowOwner;
	Second->Build(Definition(), OverflowOwner, Complete, {}, Options);
	EXPECT_TRUE(OverflowOwner.Poll());
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(IO->Work.size(), 1u);
	IO->Run(); EXPECT_EQ(Calls, 2u);
	FRequestOwner RequestOwner1;
	Second->Build(Definition(), RequestOwner1, Complete, {}, Options);
	EXPECT_EQ(IO->Work.size(), 1u);
	IO->Run(); EXPECT_EQ(Calls, 3u); EXPECT_EQ(Cache->Puts, 2u);
}

TEST(FBuildContinuationTests, ServiceDoesNotCloseInjectedCacheUsedByOtherOwners)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Async = CreateAsyncCache(Cache, IO);
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = Async});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession().value();
	FRequestOwner RequestOwner1;
	Session->Build(Definition(), RequestOwner1, [](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); });
	EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	FRequestOwner External; uint32 Calls = 0;
	const std::array Requests{FCacheGetBackendRequest{.Key = FCacheKey::FromHash(FCacheBucket::FromString("External"), FXxHash128::HashBuffer("key"))}};
	ASSERT_TRUE(Async->Get(Requests, External, [&](auto, auto Result, auto) { EXPECT_TRUE(Result); ++Calls; }));
	while (!IO->Work.empty()) IO->Run();
	EXPECT_EQ(Calls, 1u); EXPECT_EQ(External.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, SharedTaskDispatcherCompletesAcrossSessionsWithOneCoreWorker)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .CacheDispatchLimits = {.Concurrency = 1}});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	auto Second = Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("SharedCacheSingleWorker", [&] {
		FRequestOwner Owner;
		const auto Complete = [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; };
		First->Build(Definition(), Owner, Complete);
		Second->Build(Definition(), Owner, Complete);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	});
	EXPECT_EQ(Parent.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 2u); EXPECT_EQ(Cache->Gets, 2u);
	EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, InvalidCacheExecutionIsRejected)
{
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend();
	auto Rejected = Service->CreateSession({}, {}, static_cast<EBuildCacheExecution>(255));
	ASSERT_FALSE(Rejected); EXPECT_EQ(Rejected.error().Reason, EBuildAdmissionReason::InvalidRequest);
	EXPECT_TRUE(Service->CreateSession());
}

TEST(FBuildContinuationTests, IOReleasesProducerQueueAndStoreFailurePreservesLocalOutput)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>(); auto Async = CreateAsyncCache(Cache, IO, {.Concurrency = 1});
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .AsyncCache = Async});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner(ERequestPriority::High); uint32 FirstCallbacks = 0, OtherCallbacks = 0;
	Session->Build(Definition(), Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
		EXPECT_TRUE(Completion.GetOutput());
		EXPECT_TRUE(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal));
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		++FirstCallbacks;
	},
				   {}, {});
	Producer->Run();
	EXPECT_EQ(Function->Builds, 0u);
	EXPECT_EQ(IO->Work.size(), 1u);
	FRequestOwner OtherOwner;
	Session->Build(Definition(), OtherOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); ++OtherCallbacks; }, {}, Options());
	while (!Producer->Work.empty())
		Producer->Run();
	EXPECT_EQ(OtherCallbacks, 1u);
	EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run(); EXPECT_EQ(Producer->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	Producer->Run(); EXPECT_EQ(Function->Builds, 2u); EXPECT_EQ(IO->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run();
	EXPECT_EQ(FirstCallbacks, 1u);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Cache->Gets, 1u); EXPECT_EQ(Cache->Puts, 1u);
	Cache->ThrowPut = true;
	Session->Build(Definition(), Owner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(Completion.GetOutput()); ++FirstCallbacks; }, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
	Producer->Run();
	IO->Run();
	EXPECT_EQ(FirstCallbacks, 2u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	for (auto Priority : IO->Priorities) EXPECT_EQ(Priority, ERequestPriority::High);
}

TEST(FBuildContinuationTests, FullHitsSkipProducerAndPartialHitsRebuildOnce)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({Cache, CreateAsyncCache(Cache, IO)});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); uint32 Callbacks = 0;
	FRequestOwner HitOwner;
	Session->Build(Definition(), HitOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)); ++Callbacks; });
	Producer->Run();
	IO->Run();
	EXPECT_EQ(Function->Builds, 0u);
	EXPECT_EQ(Callbacks, 1u);
	EXPECT_TRUE(Producer->Work.empty());
	Cache->Partial = true;
	FRequestOwner PartialOwner;
	Session->Build(Definition(), PartialOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal)); ++Callbacks; });
	Producer->Run();
	IO->Run();
	Producer->Run();
	IO->Run();
	EXPECT_EQ(Callbacks, 2u);
	EXPECT_EQ(Function->Builds, 1u);
	EXPECT_EQ(PartialOwner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, SchemaMismatchRejectsPositiveAndNegativeHits)
{
	for (bool WrongType : {false, true}) for (bool Negative : {false, true}) for (bool BuildLocal : {false, true})
	{
		SCOPED_TRACE(std::format("WrongType={}, Negative={}, BuildLocal={}", WrongType, Negative, BuildLocal));
		auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true; Cache->Negative = Negative;
		if (WrongType) Cache->OutputSchema = "Other.Output"; else Cache->OutputVersion = 2;
		auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
		auto Function = std::make_shared<FFunction>();
		ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession().value();
		uint32 Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](auto Completion) {
			++Calls;
			EXPECT_FALSE(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit));
			EXPECT_EQ(Completion.GetStatus(), BuildLocal ? EStatus::Ok : EStatus::Error);
			EXPECT_EQ(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal), BuildLocal);
			if (BuildLocal)
			{
				ASSERT_TRUE(Completion.GetOutput());
				EXPECT_EQ(Completion.GetOutput()->GetSchema(), "Fixture.Output");
				EXPECT_EQ(Completion.GetOutput()->GetSchemaVersion(), 1u);
			}
			else
				EXPECT_FALSE(Completion.GetOutput());
		},
					   {}, {.Policy = BuildLocal ? EBuildPolicy::Default : EBuildPolicy::Cache});
		EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(Calls, 1u); EXPECT_EQ(Cache->Gets, 1u);
		EXPECT_EQ(Function->Builds, BuildLocal ? 1u : 0u); EXPECT_EQ(Cache->Puts, BuildLocal ? 1u : 0u);
	}
}

TEST(FBuildContinuationTests, MatchingCachedErrorIsRejectedAndRebuilt)
{
	for (bool BuildLocal : {false, true})
	{
		SCOPED_TRACE(BuildLocal);
		auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true; Cache->Negative = true;
		auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
		auto Function = std::make_shared<FFunction>();
		ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession().value(); uint32 Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](auto Completion) {
			++Calls;
			EXPECT_EQ(Completion.GetStatus(), BuildLocal ? EStatus::Ok : EStatus::Error);
			if (BuildLocal)
			{
				ASSERT_TRUE(Completion.GetOutput());
				EXPECT_FALSE(Completion.GetOutput()->HasError());
			}
			else
				EXPECT_FALSE(Completion.GetOutput());
			EXPECT_FALSE(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit));
			EXPECT_EQ(HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal), BuildLocal);
		},
					   {}, {.Policy = BuildLocal ? EBuildPolicy::Default : EBuildPolicy::Cache});
		EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(Calls, 1u); EXPECT_EQ(Function->Builds, BuildLocal ? 1u : 0u); EXPECT_EQ(Cache->Puts, BuildLocal ? 1u : 0u);
	}
}

TEST(FBuildContinuationTests, CancellationCloseAndDroppedContinuationCompleteOnce)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({Cache, CreateAsyncCache(Cache, IO)});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); uint32 Callbacks = 0;
	auto Callback = [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); EXPECT_FALSE(Completion.GetOutput()); ++Callbacks; };
	FRequestOwner CanceledOwner;
	Session->Build(Definition(), CanceledOwner, Callback, {}, {});
	Producer->Run();
	CanceledOwner.Cancel();
	IO->Run();
	EXPECT_TRUE(CanceledOwner.IsCanceled());
	EXPECT_EQ(Callbacks, 1u); EXPECT_EQ(Cache->Gets, 0u);
	FRequestOwner DroppedOwner;
	Session->Build(Definition(), DroppedOwner, Callback);
	Producer->Run();
	IO->Run();
	Producer->Work.clear();
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Function->Builds, 0u);
	FRequestOwner ClosedOwner;
	Session->Build(Definition(), ClosedOwner, Callback);
	Producer->Run();
	Service->Close();
	IO->Work.clear();
	EXPECT_EQ(Callbacks, 3u); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(ClosedOwner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, ReleasesCapturedResolverBeforePersistenceCompletionAndHandlesRejectedResume)
{
	class FLeaseResolver final : public IBuildInputResolver
	{
	public:
		std::shared_ptr<int> Lease;
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override { return std::vector<FBuildInputReference>{}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInput>, FBuildInputError> override { return std::vector<FBuildInput>{}; }
	};
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({Cache, CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, Producer).value(); auto Resolver = std::make_shared<FLeaseResolver>(); Resolver->Lease = std::make_shared<int>(7);
	std::weak_ptr<int> Lease = Resolver->Lease; uint32 Callbacks = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(Lease.expired()); ++Callbacks; }, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore, .InputResolver = Resolver});
	Resolver.reset();
	Producer->Run(); EXPECT_TRUE(Lease.expired());
	EXPECT_EQ(Callbacks, 0u); IO->Run(); EXPECT_EQ(Callbacks, 1u);
	FRequestOwner RejectedOwner;
	Session->Build(Definition(), RejectedOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Error); EXPECT_NE(Completion.GetCacheKey(), nullptr); ++Callbacks; });
	Producer->Run();
	Producer->Reject = true;
	IO->Run();
	EXPECT_EQ(Callbacks, 2u);
	EXPECT_EQ(RejectedOwner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, LaterStageHandleWinsWhenEarlierDispatchReturnsDuringResumeSubmission)
{
	class FHelpingWork final : public IBuildScheduledWork
	{
	public:
		mutable std::once_flag Once;
		mutable std::function<void()> Work;
		auto Wait() const -> ERequestWaitResult override { std::call_once(Once, [&] { Work(); Work = {}; }); return ERequestWaitResult::Completed; }
	};
	class FDelayedProducer final : public IBuildScheduler
	{
	public:
		uint32 Submissions = 0;
		std::promise<void> Entered;
		std::shared_future<void> Release;
		std::shared_ptr<FHelpingWork> Resume;
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work) -> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			if (++Submissions == 1) { Work(); return std::shared_ptr<IBuildScheduledWork>{}; }
			Resume = std::make_shared<FHelpingWork>(); Resume->Work = std::move(Work);
			Entered.set_value(); Release.wait(); return Resume;
		}
	};
	class FConcurrentIO final : public IBuildScheduler
	{
	public:
		std::thread Thread;
		std::shared_future<void> ResumeEntered;
		~FConcurrentIO() override { if (Thread.joinable()) Thread.join(); }
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work) -> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			Thread = std::thread(std::move(Work)); ResumeEntered.wait();
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	std::promise<void> Release; auto Producer = std::make_shared<FDelayedProducer>(); Producer->Release = Release.get_future().share();
	auto IO = std::make_shared<FConcurrentIO>(); IO->ResumeEntered = Producer->Entered.get_future().share();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({Cache, CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession({}, Producer).value();
	std::atomic<uint32> Callbacks = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); ++Callbacks; }, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheQuery});
	Release.set_value();
	auto Wait = std::async(std::launch::async, [&] { return RequestOwner.Wait(); });
	const auto Status = Wait.wait_for(std::chrono::seconds(5));
	// On regression, release the queued producer manually so diagnostics can finish.
	if (Status != std::future_status::ready) Producer->Resume->Wait();
	EXPECT_EQ(Status, std::future_status::ready); EXPECT_EQ(Wait.get(), ERequestWaitResult::Completed); EXPECT_EQ(Callbacks, 1u);
	IO->Thread.join(); EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, SlowCoreIOLeavesSingleWorkerAvailableAndDrainWaitsForCallback)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>(); std::promise<void> Entered, Release, WorkerProgress;
	auto Released = Release.get_future().share(); auto Started = Entered.get_future(); auto Progress = WorkerProgress.get_future();
	Cache->BeforeGet = [&] { Entered.set_value(); Released.wait(); };
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value(); std::atomic<uint32> Callbacks = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); ++Callbacks; });
	const auto StartStatus = Started.wait_for(std::chrono::seconds(5));
	auto Other = Tasks::LaunchTask("DDCWorkerProgress", [&] { WorkerProgress.set_value(); });
	const auto ProgressStatus = Progress.wait_for(std::chrono::seconds(5));
	Service->Close();
	EXPECT_FALSE(RequestOwner.Poll());
	Release.set_value();
	EXPECT_EQ(StartStatus, std::future_status::ready); EXPECT_EQ(ProgressStatus, std::future_status::ready);
	EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Callbacks, 1u);
	Other.Wait();
}

TEST(FBuildContinuationTests, BuildAndCacheShareOwnerAcrossNestedCallbacks)
{
	auto Producer = std::make_shared<FStageScheduler>();
	auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Async = CreateAsyncCache(Cache, IO, {.Concurrency = 1});
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .AsyncCache = Async});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		const std::array Reads{FCacheGetBackendRequest{*Result.GetCacheKey()}};
		EXPECT_TRUE(Async->Get(Reads, Owner, [&](auto, auto, auto) {
			EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
			++Calls;
		}));
		++Calls;
	});
	Producer->Run();
	EXPECT_FALSE(Owner.Poll());
	IO->Run();
	Producer->Run();
	IO->Run();
	EXPECT_FALSE(Owner.Poll());
	IO->Run();
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, ExecutionFailureDoesNotSubmitAsyncPutAndCanRetry)
{
	auto Producer = std::make_shared<FStageScheduler>();
	auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Function = std::make_shared<FFunction>();
	Function->FailExecution = true;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .AsyncCache = CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Error);
		EXPECT_EQ(Result.GetOutput(), nullptr);
		++Calls;
	});
	Producer->Run(); IO->Run(); Producer->Run();
	EXPECT_EQ(Cache->Puts.load(), 0u);
	EXPECT_TRUE(IO->Work.empty());
	EXPECT_TRUE(Owner.Poll());
	Function->FailExecution = false;
	Session->Build(Definition(), Owner, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; });
	Producer->Run(); IO->Run(); Producer->Run(); IO->Run();
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Function->Builds.load(), 2u);
	EXPECT_EQ(Cache->Puts.load(), 1u);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildSessionTests, RegistrationOnlyAffectsSessionsCreatedAfterIt)
{
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend();
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = Service->CreateSession().value();
	ASSERT_TRUE(Service->Register(std::make_shared<FSecondFunction>()));
	auto Second = Service->CreateSession().value();
	uint32 Calls = 0;
	std::optional<EStatus> MissingStatus;
	FRequestOwner MissingOwner;
	First->Build(Definition("Session.Second"), MissingOwner, [&](auto Result) { MissingStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(MissingStatus, EStatus::Error);
	FRequestOwner AcceptedOwner;
	Second->Build(Definition("Session.Second"), AcceptedOwner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Ok); }, {}, Options());
	EXPECT_TRUE(AcceptedOwner.Poll());
	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildSessionTests, PersistentSessionCompletesMultipleRequestsAndReentersWithoutLocks)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	FRequestOwner FirstOwner;
	Session->Build(Definition(), FirstOwner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		FRequestOwner NestedOwner;
	Session->Build(Definition(), NestedOwner, [&](FBuildCompleteParams Inner) {
			++Calls; EXPECT_EQ(Inner.GetStatus(), EStatus::Ok);
		}, {}, Options());
		EXPECT_TRUE(NestedOwner.Poll());
		EXPECT_EQ(Session->Drain(), EBuildDrainResult::WouldBlock); }, {}, Options());
	EXPECT_TRUE(FirstOwner.Poll());
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Fixture.Function->Builds.load(), 2u);
}

TEST(FBuildSessionTests, RejectedDispatchAndDroppedWorkCompleteOnce)
{
	for (uint32 Mode = 0; Mode < 2; ++Mode)
	{
		FFixture Fixture;
		auto Session = Fixture.Session([=](auto) -> std::expected<void, FBuildAdmissionError> {
			if (Mode == 0) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, std::string(5000, 'x')});
			return {};
		});
		uint32 Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), Mode == 0 ? EStatus::Error : EStatus::Canceled);
            if (Mode == 0) EXPECT_LE(Result.GetDiagnostic().size(), FBuildAdmissionError::MaximumDescriptionBytes); }, {}, Options());
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
		EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	}
}

TEST(FBuildSessionTests, DroppedDispatchRacesAcceptanceCompletesExactlyOnce)
{
	FFixture Fixture;
	std::barrier Start(2);
	std::jthread Dropper;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Dropper = std::jthread([Work = std::move(Work), &Start]() mutable {
			Start.arrive_and_wait();
			Work = {};
		});
		Start.arrive_and_wait();
		return {};
	});
	for (uint32 Round = 0; Round < 50; ++Round)
	{
		std::atomic<uint32> Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
			++Calls; }, {}, Options());
		Dropper.join();
		EXPECT_TRUE(RequestOwner.Poll());
		EXPECT_EQ(Calls.load(), 1u);
	}
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildSessionTests, CapacityRejectionCompletesOnceWithoutScheduling)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queue.push_back(std::move(Work)); return {};
	});
	uint32 Calls = 0;
	FRequestOwner Owner;
	for (uint32 Index = 0; Index < 4096; ++Index)
		Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); }, {}, Options());
	std::optional<EStatus> OverflowStatus;
	FRequestOwner OverflowOwner;
	Session->Build(Definition(), OverflowOwner, [&](auto Result) { OverflowStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(OverflowStatus, EStatus::Error);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Calls, 4097u);
}

TEST(FBuildSessionTests, QueuedCancellationAndRunnerRaceCompleteExactlyOnce)
{
	FFixture Fixture;
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queued = std::move(Work); return {};
	});
	for (uint32 Round = 0; Round < 50; ++Round)
	{
		std::atomic<uint32> Calls = 0;
		std::atomic<bool> Canceled = false;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), RequestOwner, [&](FBuildCompleteParams Result) {
			Canceled = Result.GetStatus() == EStatus::Canceled; ++Calls; }, {}, Options());
		std::barrier Start(2);
		std::jthread Worker([&] { Start.arrive_and_wait(); Queued(); });
		Start.arrive_and_wait();
		RequestOwner.Cancel();
		Worker.join();
		EXPECT_TRUE(RequestOwner.Poll());
		EXPECT_EQ(Calls.load(), 1u);
		EXPECT_TRUE(RequestOwner.IsCanceled());
		Queued = {};
	}
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildSessionTests, CloseCancelsQueuedRequestsAndStaleThunksAreHarmless)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queue.push_back(std::move(Work)); return {};
	});
	uint32 Calls = 0;
	FRequestOwner Owner;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); }, {}, Options());
	}
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained); EXPECT_EQ(Calls, 16u);
	EXPECT_TRUE(Owner.Poll());
	for (auto& Work : Queue) Work();
	EXPECT_EQ(Calls, 16u); EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, RunningCloseDrainsCallbackBeforeServiceRelease)
{
	FFixture Fixture;
	FThreadEvent Started, Release, CallbackFinished;
	Fixture.Function->OnBuild = [&] { Started.Trigger(); EXPECT_TRUE(Release.WaitFor(2.0)); };
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queued = std::move(Work); return {};
	});
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); CallbackFinished.Trigger(); }, {}, Options());
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(Started.WaitFor(2.0)); Fixture.Service->Close();
	EXPECT_FALSE(RequestOwner.Poll());
	Release.Trigger();
	EXPECT_EQ(Fixture.Service->Drain(), EBuildDrainResult::Drained);
	EXPECT_TRUE(CallbackFinished.WaitFor(0.0));
	Worker.join();
	EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(RequestOwner.Poll());
}

TEST(FBuildSessionTests, ServiceDrainTracksStateAfterLastSessionHandleDiesInCallback)
{
	FFixture Fixture;
	FThreadEvent CallbackStarted, ReleaseCallback, DrainReturned;
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queued = std::move(Work); return {};
	});
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		Session.reset();
		CallbackStarted.Trigger();
		EXPECT_TRUE(ReleaseCallback.WaitFor(2.0)); }, {}, Options());
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(CallbackStarted.WaitFor(2.0));
	std::jthread Drainer([&] {
		EXPECT_EQ(Fixture.Service->Drain(), EBuildDrainResult::Drained);
		DrainReturned.Trigger();
	});
	EXPECT_FALSE(DrainReturned.WaitFor(0.05));
	ReleaseCallback.Trigger();
	EXPECT_TRUE(DrainReturned.WaitFor(2.0));
	Worker.join(); Drainer.join();
	EXPECT_TRUE(RequestOwner.Poll());
}

TEST(FBuildSessionTests, AdmissionErrorsCompleteExactlyOnce)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	std::optional<EStatus> MissingFunctionStatus;
	FRequestOwner MissingFunctionOwner;
	Session->Build(Definition("Missing.Function"), MissingFunctionOwner, [&](auto Result) { MissingFunctionStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(MissingFunctionStatus, EStatus::Error);
	FBuildFunctionDescriptor Mismatch{
		.Name = "Session.Fixture",
		.Version = 1,
		.ConstantsSchema = 1,
		.OutputType = "Fixture.Output",
		.OutputSchema = 1,
		.Bucket = FCacheBucket::FromString("Sessions")};
	++Mismatch.Version;
	auto InvalidAction = std::move(FBuildActionBuilder(Definition(), std::move(Mismatch))).Build().value();
	std::optional<EStatus> InvalidRequestStatus;
	FRequestOwner InvalidRequestOwner;
	Session->Build(std::move(InvalidAction), InvalidRequestOwner, [&](auto Result) { InvalidRequestStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(InvalidRequestStatus, EStatus::Error);
	Session->Close();
	std::optional<EStatus> ClosedStatus;
	FRequestOwner ClosedOwner;
	Session->Build(Definition(), ClosedOwner, [&](auto Result) { ClosedStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(ClosedStatus, EStatus::Error);
	EXPECT_EQ(Calls, 3u);
}

TEST(FBuildSessionTests, CallbackExceptionsDoNotBreakCompletionOrDrainAccounting)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [](FBuildCompleteParams) { throw std::runtime_error("callback"); }, {}, Options());
	EXPECT_TRUE(RequestOwner.Poll());
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
}

TEST(FBuildSessionTests, RequestWaitIncludesCallbackAndDispatchReturn)
{
	FFixture Fixture;
	FThreadEvent CallbackStarted, ReleaseCallback, DispatchStarted, ReleaseDispatch, WaitReturned;
	std::function<void()> Queued;
	FRequestOwner Owner;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queued = std::move(Work);
		DispatchStarted.Trigger();
		EXPECT_TRUE(ReleaseDispatch.WaitFor(2.0));
		return {};
	});
	std::jthread Submitter([&] {
		Session->Build(Definition(), Owner, [&](auto) {
			CallbackStarted.Trigger();
			EXPECT_TRUE(ReleaseCallback.WaitFor(2.0)); }, {}, Options());
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	});
	ASSERT_TRUE(DispatchStarted.WaitFor(2.0));
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(CallbackStarted.WaitFor(2.0));
	std::jthread Waiter([&] { EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); WaitReturned.Trigger(); });
	EXPECT_FALSE(WaitReturned.WaitFor(0.05));
	ReleaseCallback.Trigger();
	Worker.join();
	EXPECT_FALSE(WaitReturned.WaitFor(0.05));
	ReleaseDispatch.Trigger();
	Submitter.join(); Waiter.join();
	EXPECT_TRUE(Owner.Poll());
	// Waiting did not close the reusable session.
	FRequestOwner RequestOwner1;
	Session->Build(Definition(), RequestOwner1, [](auto) {}, {}, Options());
	Queued();
}

TEST(FBuildSessionTests, OwnerCancellationCoversExistingAndFutureRequestsAcrossSessions)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	auto First = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> { Queue.push_back(std::move(Work)); return {}; });
	auto Second = Fixture.Session();
	FRequestOwner Owner;
	uint32 Calls = 0;
	auto Completed = [&](FBuildCompleteParams Result) { ++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); };
	First->Build(Definition(), Owner, Completed, {}, Options());
	EXPECT_FALSE(Owner.Poll());
	Owner.Cancel();
	Second->Build(Definition(), Owner, Completed, {}, Options());
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_TRUE(Owner.IsCanceled());
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	Queue.clear();
}

TEST(FBuildSessionTests, OwnerDestructionCancelsQueuedWorkAndRejectionLeavesNoAccounting)
{
	FFixture Fixture;
	std::function<void()> Stale;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> { Stale = std::move(Work); return {}; });
	uint32 Calls = 0;
	{
		FRequestOwner Owner;
		Session->Build(Definition("Missing"), Owner, [](auto) {}, {}, Options());
		EXPECT_TRUE(Owner.Poll());
		Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); }, {}, Options());
	}
	EXPECT_EQ(Calls, 1u);
	Stale();
	EXPECT_EQ(Calls, 1u);
}

TEST(FBuildSessionTests, RequestAndOwnerSelfWaitReturnWouldBlock)
{
	FFixture Fixture;
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> { Queued = std::move(Work); return {}; });
	FRequestOwner Owner;

	Session->Build(Definition(), Owner, [&](auto) {
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock); }, {}, Options());

	Queued();
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, TaskSchedulerWaitHelpsNestedBuildWithOneWorker)
{
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("NestedDerivedData", [&] {
		FRequestOwner Owner(ERequestPriority::High);
		Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; }, {}, Options());
		EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
}

TEST(FBuildSessionTests, StoppedTaskSchedulerCompletesWithError)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	std::optional<EStatus> RequestStatus;
	Session->Build(Definition(), Owner, [&](auto Result) { RequestStatus = Result.GetStatus(); ++Calls; }, {}, Options());
	ASSERT_EQ(RequestStatus, EStatus::Error);
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, SchedulerReceivesOwnerPriority)
{
	class FRecordingScheduler final : public IBuildScheduler
	{
	public:
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			EXPECT_EQ(Params.FunctionName, "Session.Fixture");
			EXPECT_EQ(Params.Priority, ERequestPriority::Low);

			Work();
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, std::make_shared<FRecordingScheduler>()).value();
	FRequestOwner Owner(ERequestPriority::Low);
	auto Policy = Options();
	Session->Build(Definition(), Owner, [](auto) {}, {}, std::move(Policy));
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, ReusedOptionsCannotShareCancellationAcrossOwners)
{
	FFixture Fixture;
	auto Scheduler = std::make_shared<FStageScheduler>();
	auto Session = Fixture.Service->CreateSession({}, Scheduler).value();
	FRequestOwner First, Second;
	const auto SharedOptions = Options();
	uint32 Canceled = 0, Completed = 0;
	Fixture.Function->OnBuild = [&] { First.RequestCancel(); };
	Session->Build(Definition(), First, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		EXPECT_FALSE(Result.GetOutput());
		++Canceled;
	}, {}, SharedOptions);
	Scheduler->Run();
	EXPECT_EQ(First.Wait(), ERequestWaitResult::Completed);
	EXPECT_FALSE(Second.IsCanceled());
	Session->Build(Definition(), Second, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		EXPECT_TRUE(Result.GetOutput());
		++Completed;
	}, {}, SharedOptions);
	Scheduler->Run();
	EXPECT_EQ(Second.Wait(), ERequestWaitResult::Completed);
	Session->Build(Definition(), First, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		++Canceled;
	}, {}, SharedOptions);
	EXPECT_EQ(First.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Canceled, 2u);
	EXPECT_EQ(Completed, 1u);
	EXPECT_EQ(Fixture.Function->Builds, 2u);
}

TEST(FBuildSessionTests, UnsupportedPolicyCompletesOnceWithoutDispatch)
{
	FFixture Fixture;
	auto Scheduler = std::make_shared<FStageScheduler>();
	auto Session = Fixture.Service->CreateSession({}, Scheduler).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Error);
		EXPECT_FALSE(Result.GetDiagnostic().empty());
		++Calls;
	}, {}, {.Policy = static_cast<EBuildPolicy>(1 << 7)});
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Scheduler->Work.empty());
	EXPECT_EQ(Fixture.Function->Builds, 0u);
}

TEST(FBuildSessionTests, CanceledOwnerDoesNotScheduleNewWork)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	FRequestOwner Owner; Owner.Cancel();
	uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); }, {}, Options());
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, EmptyOwnerIsAlreadyComplete)
{
	FRequestOwner Owner;
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, OwnerMayRequestCancellationInsideItsCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	auto Owner = std::make_unique<FRequestOwner>();
	uint32 Calls = 0;
	Session->Build(Definition(), *Owner, [&](auto) {
		++Calls; Owner->RequestCancel(); }, {}, Options());
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ((*Owner).Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildSessionDeathTests, OwnerDestructionInsideCompletionIsRejected)
{
	EXPECT_DEATH({
		FFixture Fixture; auto Session = Fixture.Session(); auto Owner = std::make_unique<FRequestOwner>();
		(void)Session->Build(Definition(), *Owner, [&](auto) { Owner.reset(); }, {}, Options());
	}, "must be drained");
}

TEST(FBuildContinuationTests, InlineNestedBuildCompletesBeforeOuterCallbackReturns)
{
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession().value(); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
		++Calls;
		Session->Build(Definition(), Owner, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; });
		EXPECT_FALSE(Owner.Poll());
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		EXPECT_EQ(Calls, 2u);
	});
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildContinuationTests, CancelRetiresQueuedContinuationWithoutProducerProgress)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({Cache, CreateAsyncCache(Cache, IO)});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); ++Calls; });
	Producer->Run();
	IO->Run();
	ASSERT_EQ(Producer->Work.size(), 1u);
	Owner.Cancel();
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Owner.Poll());
	EXPECT_TRUE(Owner.Poll());
	Producer->Run(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Function->Builds, 0u);
}

TEST(FBuildSessionTests, SynchronousCancelWaitsForRunningCallbackAndCaptureRelease)
{
	FFixture Fixture; std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> { Queued = std::move(Work); return {}; });
	FThreadEvent Entered, Release, CancelStarted, CancelReturned;
	auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [Capture, &Entered, &Release](auto) { Entered.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0)); }, {}, Options());
	Capture.reset();
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(Entered.WaitFor(5.0));
	std::jthread Canceler([&] { CancelStarted.Trigger(); RequestOwner.Cancel(); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(RequestOwner.Poll()); CancelReturned.Trigger(); });
	ASSERT_TRUE(CancelStarted.WaitFor(5.0)); EXPECT_FALSE(CancelReturned.WaitFor(0.05));
	Release.Trigger(); Worker.join(); Canceler.join(); EXPECT_TRUE(CancelReturned.WaitFor(0.0));
}

TEST(FBuildSessionDeathTests, SynchronousOwnerCancellationInsideCallbackIsRejected)
{
	EXPECT_DEATH({
		FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
		(void)Session->Build(Definition(), Owner, [&](auto) { Owner.Cancel(); }, {}, Options());
	}, "use RequestCancel");
}

TEST(FBuildContinuationTests, CompletionWaitsForPersistenceAndRetiresCaptures)
{
	for (uint32 StoreMode = 0; StoreMode < 3; ++StoreMode)
	{
		SCOPED_TRACE(StoreMode);
		auto Producer = std::make_shared<FStageScheduler>(), IO = std::make_shared<FStageScheduler>();
		auto Cache = std::make_shared<FContinuationCache>();
		Cache->FailPut = StoreMode == 1; Cache->ThrowPut = StoreMode == 2;
		auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO)});
		auto Function = std::make_shared<FFunction>();
		ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession({}, Producer).value();
		FRequestOwner Owner; uint32 Calls = 0; std::optional<FBuildOutput> Delivered;
		auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
		Cache->BeforePut = [&] { EXPECT_EQ(Calls, 0u); EXPECT_FALSE(Weak.expired()); };
		Session->Build(Definition(), Owner, [&, Capture](auto Completion) {
			++Calls;
			EXPECT_EQ(Cache->Puts, 1u);
			EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
			ASSERT_TRUE(Completion.GetOutput());
			Delivered = *Completion.GetOutput();
			EXPECT_FALSE(Weak.expired());
			EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		}, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
		Capture.reset(); Producer->Run();
		EXPECT_EQ(Calls, 0u); EXPECT_FALSE(Weak.expired()); EXPECT_FALSE(Owner.Poll());
		ASSERT_EQ(IO->Work.size(), 1u); EXPECT_EQ(Cache->Puts, 0u);
		FThreadEvent Waiting, Retired;
		std::jthread Waiter([&] { Waiting.Trigger(); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); Retired.Trigger(); });
		Waiting.Wait(); EXPECT_FALSE(Retired.WaitFor(0.02));
		IO->Run(); Waiter.join();
		EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(Owner.Poll());
		ASSERT_TRUE(Delivered); EXPECT_FALSE(Delivered->HasError());
		ASSERT_NE(Delivered->FindValue(FValueId::FromName("Data")), nullptr);
		EXPECT_EQ(Delivered->FindValue(FValueId::FromName("Data"))->GetRawSize(), 32u);
		EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	}
}

TEST(FBuildContinuationTests, CancellationWhileStoreQueuedDiscardsLocalResultOnce)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO)});
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession().value(); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), Owner, [&](auto Completion) {
		++Calls; EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); EXPECT_FALSE(Completion.GetOutput());
	}, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
	EXPECT_EQ(Function->Builds, 1u); EXPECT_EQ(Calls, 0u); ASSERT_EQ(IO->Work.size(), 1u);
	Owner.Cancel();
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Cache->Puts, 0u);
	IO->Run(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Cache->Puts, 0u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, CancellationWhileStoreRunningWaitsForStoreRetirement)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->FailPut = false;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession().value();
	FThreadEvent Entered, Release; FRequestOwner Owner; std::atomic<uint32> Calls = 0;
	auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
	Cache->BeforePut = [&] { Entered.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0)); };
	Session->Build(Definition(), Owner, [&, Capture](auto Completion) {
		EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); EXPECT_FALSE(Completion.GetOutput()); ++Calls;
	}, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
	Capture.reset(); EXPECT_EQ(Calls.load(), 0u); ASSERT_EQ(IO->Work.size(), 1u);
	std::jthread Worker([&] { IO->Run(); });
	const bool Started = Entered.WaitFor(5.0);
	EXPECT_TRUE(Started);
	Owner.RequestCancel();
	EXPECT_EQ(Calls.load(), 0u); EXPECT_FALSE(Owner.Poll()); EXPECT_FALSE(Weak.expired());
	Release.Trigger(); Worker.join();
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls.load(), 1u); EXPECT_EQ(Cache->Puts, 1u); EXPECT_TRUE(Weak.expired());
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, CloseWhileStorePendingCompletesCanceledOnce)
{
	for (bool CloseService : {false, true})
	{
		SCOPED_TRACE(CloseService);
		auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
		auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO)});
		ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession().value();
		FRequestOwner Owner; uint32 Calls = 0;
		Session->Build(Definition(), Owner, [&](auto Completion) {
			++Calls; EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); EXPECT_FALSE(Completion.GetOutput());
		}, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
		EXPECT_EQ(Calls, 0u); ASSERT_EQ(IO->Work.size(), 1u);
		if (CloseService) Service->Close(); else Session->Close();
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Calls, 1u);
		IO->Run(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Cache->Puts, 0u);
		EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	}
}

TEST(FBuildContinuationTests, CompletionCanCloseSessionAfterPersistenceResponse)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.AsyncCache = CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession().value();
	uint32 Calls = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), RequestOwner, [&](auto Completion) {
		++Calls;
		EXPECT_EQ(Cache->Puts, 1u);
		EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
		Session->Close();
		EXPECT_EQ(Session->Drain(), EBuildDrainResult::WouldBlock);
	}, {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
	EXPECT_EQ(Calls, 0u);
	while (!IO->Work.empty()) IO->Run();
	EXPECT_EQ(RequestOwner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, CompletionAndPersistenceDrainRemainLiveWithOneWorker)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, CreateTaskBuildScheduler(), EBuildCacheExecution::Task).value();
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("BuildCompletionSingleWorker", [&] {
		FRequestOwner Owner;
		std::optional<EStatus> RequestStatus;
		Session->Build(Definition(), Owner, [&](auto Result) {
			RequestStatus = Result.GetStatus();
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
			++Calls;
		},
					   {}, {.Policy = EBuildPolicy::Build | EBuildPolicy::CacheStore});
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
		EXPECT_TRUE(RequestStatus.has_value());
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 1u); EXPECT_EQ(Cache->Puts, 1u); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, DiagnosticsCountRebuildFallbackAndSchemaRejectionWithoutChangingCompletion)
{
	Durin::DerivedData::Private::ResetCacheDiagnosticsForTests();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession().value();
	uint32 Calls = 0;
	FRequestOwner RequestOwner3;
	Session->Build(Definition(), RequestOwner3, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; });
	Cache->Hit = true; Cache->OutputSchema = "Wrong.Schema";
	FRequestOwner RequestOwner2;
	Session->Build(Definition(), RequestOwner2, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; });
	FRequestOwner RequestOwner1;
	Session->Build(Definition(), RequestOwner1, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Error); ++Calls; }, {}, {.Policy = EBuildPolicy::Cache});
	const auto Stats = GetCacheDiagnostics();
	EXPECT_EQ(Stats.Total.BuildFallbacks, 2u); EXPECT_EQ(Stats.Total.SchemaRejections, 2u); EXPECT_EQ(Calls, 3u);
}

TEST(FBuildSessionDeathTests, MissingCompletionIsRejected)
{
	EXPECT_DEATH({ FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
  Session->Build(Definition(), Owner, {}); }, "Completion");
}

TEST(FRequestOwnerTests, PublicLaunchRetiresNestedWorkWithOneWorker)
{
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown
	{
		~FShutdown() { ShutdownTaskScheduler(true); }
	} Shutdown;
	std::atomic_uint32_t Calls = 0;
	auto Parent = Tasks::LaunchTask("OwnerPublicLaunchParent", [&] {
		FRequestOwner Owner;
		Owner.LaunchTask("OwnerPublicLaunchChild", [&] { ++Calls; EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock); });
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls, 1u);
}

TEST(FRequestOwnerTests, PublicLaunchStillInvokesBodyForCanceledOwnerAndStoppedScheduler)
{
	ShutdownTaskScheduler(false);
	FRequestOwner Owner;
	uint32 Calls = 0;
	Owner.LaunchTask("StoppedOwnerTask", [&] { ++Calls; EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock); });
	Owner.Cancel();
	Owner.LaunchTask("CanceledOwnerTask", [&] { ++Calls; EXPECT_TRUE(Owner.IsCanceled()); });
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 2u);
}

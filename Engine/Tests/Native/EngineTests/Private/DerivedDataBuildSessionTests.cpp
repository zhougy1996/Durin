#include "DerivedDataBuildSession.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskComposition.h"
#include "DerivedDataCache/DerivedDataCacheAsync.h"
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
		bool FailExecution = false;
		auto GetName() const -> std::string_view override { return "Session.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& Context) const -> void override
		{ Context.SetConstantsSchema(1); Context.SetOutput("Fixture.Output", 1); Context.SetCacheBucket(FCacheBucket::FromString("Sessions")); }
		auto Build(FBuildContext& Context) const -> void override
		{
			++Builds; if (OnBuild) OnBuild();
			if (FailExecution) return Context.FailExecution("temporary execution failure");
			Context.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(FByteBuffer(32, std::byte{7})));
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
	{ return {.Policy = {.QueryCache = false, .StoreOnBuild = false}}; }
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
		std::shared_ptr<IBuild> Service = CreateBuild();
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
	class FContinuationCache final : public ICache
	{
	public:
		mutable std::atomic<uint32> Gets = 0, Puts = 0;
		bool Hit = false, Partial = false, ThrowPut = false;
		std::function<void()> BeforeGet;
		auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult override
		{
			++Gets; if (BeforeGet) BeforeGet(); if (!Hit) return std::optional<FCacheRecord>{};
			FBuildOutputBuilder Builder("Fixture.Output", 1); Builder.AddValue(FValueId::FromName("Data"), FSharedByteBuffer::Take(FByteBuffer(32, std::byte{7})));
			auto Record = FCacheRecord::FromOutput(Request.Key, std::move(Builder).Build().value()).value();
			return std::optional{Partial ? Record.RemoveValueData() : Record};
		}
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
		{
			++Puts; EXPECT_TRUE(Request.Record.ToOutput(Request.Record.GetKey()));
			if (ThrowPut) throw std::runtime_error("fixture persistence exception");
			return std::unexpected(FCacheError{ECacheError::StorageFailure, "fixture failed persistence"});
		}
	};
}

TEST(FBuildContinuationTests, ExplicitCacheSchedulerDoesNotChangeInlineSessions)
{
	auto Cache = std::make_shared<FContinuationCache>(); auto Service = CreateBuild({.Cache = Cache});
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto ExistingInline = Service->CreateSession({}, CreateInlineBuildScheduler()).value();
	auto TaskSession = Service->CreateSession({}, CreateTaskBuildScheduler(), CreateTaskCacheScheduler()).value();
	auto LaterInline = Service->CreateSession({}, CreateInlineBuildScheduler()).value();
	const auto Caller = std::this_thread::get_id(); uint32 Callbacks = 0;
	Function->OnBuild = [&] { EXPECT_EQ(std::this_thread::get_id(), Caller); };
	for (const auto& Session : {ExistingInline, LaterInline})
	{
		auto Request = Session->Build(Definition(), [&](auto Completion) {
			EXPECT_EQ(std::this_thread::get_id(), Caller); EXPECT_EQ(Completion.GetStatus(), EStatus::Ok);
			EXPECT_TRUE(Completion.GetOutput()); ++Callbacks;
		});
		ASSERT_TRUE(Request); EXPECT_EQ(Request->Wait(), ERequestWaitResult::Completed);
	}
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Cache->Gets, 2u); EXPECT_EQ(Cache->Puts, 2u);
	EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildContinuationTests, IOReleasesProducerQueueAndStoreFailurePreservesLocalOutput)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>(); auto Async = CreateAsyncCache(Cache, IO, 1);
	auto Service = CreateBuild({.Cache = Cache, .AsyncCache = Async}); auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner(ERequestPriority::High); uint32 FirstCallbacks = 0, OtherCallbacks = 0;
	auto First = Session->Build(Definition(), Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(Completion.GetOutput());
		EXPECT_TRUE(HasBuildStatus(Completion.GetBuildStatus(), EBuildStatus::BuildLocal));
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock); ++FirstCallbacks;
	}, {}, {.Policy = {.MaximumWorkingSetBytes = 123456}});
	ASSERT_TRUE(First); Producer->Run(); EXPECT_EQ(Function->Builds, 0u); EXPECT_EQ(IO->Work.size(), 1u);
	auto Other = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); ++OtherCallbacks; }, {}, Options()); ASSERT_TRUE(Other);
	while (!Producer->Work.empty()) Producer->Run(); EXPECT_EQ(OtherCallbacks, 1u); EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run(); EXPECT_EQ(Producer->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	Producer->Run(); EXPECT_EQ(Function->Builds, 2u); EXPECT_EQ(IO->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run(); EXPECT_EQ(FirstCallbacks, 1u); EXPECT_EQ(First->Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Cache->Gets, 1u); EXPECT_EQ(Cache->Puts, 1u);
	Cache->ThrowPut = true;
	auto Throwing = Session->Build(Definition(), Owner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(Completion.GetOutput()); ++FirstCallbacks; }, {}, {.Policy = {.QueryCache = false, .MaximumWorkingSetBytes = 123456}});
	ASSERT_TRUE(Throwing); Producer->Run(); IO->Run(); EXPECT_EQ(FirstCallbacks, 2u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	for (auto Priority : IO->Priorities) EXPECT_EQ(Priority, ERequestPriority::High);
}

TEST(FBuildContinuationTests, FullHitsSkipProducerAndPartialHitsRebuildOnce)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = CreateBuild({Cache, CreateAsyncCache(Cache, IO)}); auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); uint32 Callbacks = 0;
	auto Hit = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildStatus(Completion.GetBuildStatus(), EBuildStatus::CacheQueryHit)); ++Callbacks; }); ASSERT_TRUE(Hit);
	Producer->Run(); IO->Run(); EXPECT_EQ(Function->Builds, 0u); EXPECT_EQ(Callbacks, 1u); EXPECT_TRUE(Producer->Work.empty());
	Cache->Partial = true;
	auto Partial = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildStatus(Completion.GetBuildStatus(), EBuildStatus::BuildLocal)); ++Callbacks; }); ASSERT_TRUE(Partial);
	Producer->Run(); IO->Run(); Producer->Run(); IO->Run(); EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Function->Builds, 1u);
	EXPECT_EQ(Partial->Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildContinuationTests, CancellationCloseAndDroppedContinuationCompleteOnce)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({Cache, CreateAsyncCache(Cache, IO)}); auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); uint32 Callbacks = 0;
	auto Callback = [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); EXPECT_FALSE(Completion.GetOutput()); ++Callbacks; };
	auto Canceled = Session->Build(Definition(), Callback); ASSERT_TRUE(Canceled); Producer->Run(); Canceled->Cancel(); IO->Run();
	EXPECT_EQ(Callbacks, 1u); EXPECT_EQ(Cache->Gets, 0u);
	auto Dropped = Session->Build(Definition(), Callback); ASSERT_TRUE(Dropped); Producer->Run(); IO->Run(); Producer->Work.clear();
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Function->Builds, 0u);
	auto Closed = Session->Build(Definition(), Callback); ASSERT_TRUE(Closed); Producer->Run(); Service->Close(); IO->Work.clear();
	EXPECT_EQ(Callbacks, 3u); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Closed->Wait(), ERequestWaitResult::Completed);
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
	auto Service = CreateBuild({Cache, CreateAsyncCache(Cache, IO)}); ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, Producer).value(); auto Resolver = std::make_shared<FLeaseResolver>(); Resolver->Lease = std::make_shared<int>(7);
	std::weak_ptr<int> Lease = Resolver->Lease; uint32 Callbacks = 0;
	auto Request = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); EXPECT_TRUE(Lease.expired()); ++Callbacks; }, {},
		{.Policy = {.QueryCache = false}, .InputResolver = Resolver}); ASSERT_TRUE(Request); Resolver.reset();
	Producer->Run(); EXPECT_TRUE(Lease.expired());
	EXPECT_EQ(Callbacks, 0u); IO->Run(); EXPECT_EQ(Callbacks, 1u);
	auto Rejected = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Error); EXPECT_NE(Completion.GetCacheKey(), nullptr); ++Callbacks; }); ASSERT_TRUE(Rejected);
	Producer->Run(); Producer->Reject = true; IO->Run(); EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Rejected->Wait(), ERequestWaitResult::Completed);
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
	auto Cache = std::make_shared<FContinuationCache>(); auto Service = CreateBuild({Cache, CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = Service->CreateSession({}, Producer).value();
	std::atomic<uint32> Callbacks = 0;
	auto Request = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); ++Callbacks; }, {}, {.Policy = {.StoreOnBuild = false}});
	Release.set_value(); ASSERT_TRUE(Request);
	auto Wait = std::async(std::launch::async, [&] { return Request->Wait(); });
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
	auto Service = CreateBuild({.Cache = Cache}); ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, CreateTaskBuildScheduler(), CreateTaskCacheScheduler()).value(); std::atomic<uint32> Callbacks = 0;
	auto Request = Session->Build(Definition(), [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); ++Callbacks; }); ASSERT_TRUE(Request);
	const auto StartStatus = Started.wait_for(std::chrono::seconds(5));
	auto Other = Tasks::LaunchTask("DDCWorkerProgress", [&] { WorkerProgress.set_value(); });
	const auto ProgressStatus = Progress.wait_for(std::chrono::seconds(5));
	Service->Close(); EXPECT_FALSE(Request->IsComplete()); Release.set_value();
	EXPECT_EQ(StartStatus, std::future_status::ready); EXPECT_EQ(ProgressStatus, std::future_status::ready);
	EXPECT_EQ(Request->Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained); EXPECT_EQ(Callbacks, 1u);
	Other.Wait();
}

TEST(FBuildContinuationTests, BuildAndCacheShareOwnerAcrossNestedCallbacks)
{
	auto Producer = std::make_shared<FStageScheduler>();
	auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Async = CreateAsyncCache(Cache, IO, 1);
	auto Service = CreateBuild({.Cache = Cache, .AsyncCache = Async});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	auto Build = Session->Build(Definition(), Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
		const std::array Reads{FCacheGetRequest{*Result.GetCacheKey()}};
		EXPECT_TRUE(Async->Get(Reads, Owner, [&](auto, auto, auto) {
			EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
			++Calls;
		}));
		++Calls;
	});
	ASSERT_TRUE(Build);
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
	auto Service = CreateBuild({.Cache = Cache, .AsyncCache = CreateAsyncCache(Cache, IO)});
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	ASSERT_TRUE(Session->Build(Definition(), Owner, [&](auto Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Error);
		EXPECT_EQ(Result.GetOutput(), nullptr);
		++Calls;
	}));
	Producer->Run(); IO->Run(); Producer->Run();
	EXPECT_EQ(Cache->Puts.load(), 0u);
	EXPECT_TRUE(IO->Work.empty());
	EXPECT_TRUE(Owner.Poll());
	Function->FailExecution = false;
	ASSERT_TRUE(Session->Build(Definition(), Owner, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; }));
	Producer->Run(); IO->Run(); Producer->Run(); IO->Run();
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Function->Builds.load(), 2u);
	EXPECT_EQ(Cache->Puts.load(), 1u);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildSessionTests, RegistrationOnlyAffectsSessionsCreatedAfterIt)
{
	auto Service = CreateBuild();
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = Service->CreateSession().value();
	ASSERT_TRUE(Service->Register(std::make_shared<FSecondFunction>()));
	auto Second = Service->CreateSession().value();
	uint32 Calls = 0;
	auto Missing = First->Build(Definition("Session.Second"), [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().Reason, EBuildAdmissionReason::MissingFunction);
	auto Accepted = Second->Build(Definition("Session.Second"), [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
	}, {}, Options());
	ASSERT_TRUE(Accepted);
	EXPECT_TRUE(Accepted->IsComplete());
	EXPECT_EQ(Calls, 1u);
}

TEST(FBuildSessionTests, PersistentSessionCompletesMultipleRequestsAndReentersWithoutLocks)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	auto First = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		auto Nested = Session->Build(Definition(), [&](FBuildCompleteParams Inner) {
			++Calls; EXPECT_EQ(Inner.GetStatus(), EStatus::Ok);
		}, {}, Options());
		EXPECT_TRUE(Nested); EXPECT_TRUE(Nested->IsComplete());
		EXPECT_EQ(Session->Drain(), EBuildDrainResult::WouldBlock);
	}, {}, Options());
	ASSERT_TRUE(First); EXPECT_TRUE(First->IsComplete()); EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Fixture.Function->Builds.load(), 2u);
}

TEST(FBuildSessionTests, RejectedDispatchHasNoCompletionAndDroppedWorkCancels)
{
	for (uint32 Mode = 0; Mode < 2; ++Mode)
	{
		FFixture Fixture;
		auto Session = Fixture.Session([=](auto) -> std::expected<void, FBuildAdmissionError> {
			if (Mode == 0) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, std::string(5000, 'x')});
			return {};
		});
		uint32 Calls = 0;
		auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		}, {}, Options());
		EXPECT_EQ(Request.has_value(), Mode == 1);
		if (!Request)
		{
			EXPECT_EQ(Request.error().Reason, EBuildAdmissionReason::DispatchRejected);
			EXPECT_EQ(Request.error().Description.size(), FBuildAdmissionError::MaximumDescriptionBytes);
		}
		EXPECT_EQ(Calls, Mode == 1 ? 1u : 0u);
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
		auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
			++Calls;
		}, {}, Options());
		Dropper.join();
		ASSERT_TRUE(Request);
		EXPECT_TRUE(Request->IsComplete());
		EXPECT_EQ(Calls.load(), 1u);
	}
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildSessionTests, CapacityRejectionIsTypedAndInvokesNoCompletion)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queue.push_back(std::move(Work)); return {};
	});
	uint32 Calls = 0;
	for (uint32 Index = 0; Index < 4096; ++Index)
		ASSERT_TRUE(Session->Build(Definition(), [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		}, {}, Options()));
	auto Overflow = Session->Build(Definition(), [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(Overflow);
	EXPECT_EQ(Overflow.error().Reason, EBuildAdmissionReason::Capacity);
	EXPECT_EQ(Calls, 0u);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Calls, 4096u);
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
		auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
			Canceled = Result.GetStatus() == EStatus::Canceled; ++Calls;
		}, {}, Options());
		ASSERT_TRUE(Request);
		std::barrier Start(2);
		std::jthread Worker([&] { Start.arrive_and_wait(); Queued(); });
		Start.arrive_and_wait(); const bool AcceptedCancel = Request->Cancel(); Worker.join();
		EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls.load(), 1u);
		EXPECT_EQ(Canceled.load(), AcceptedCancel);
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
	std::vector<FBuildRequest> Requests;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		}, {}, Options());
		ASSERT_TRUE(Request); Requests.push_back(*Request);
	}
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained); EXPECT_EQ(Calls, 16u);
	for (const auto& Request : Requests) EXPECT_TRUE(Request.IsComplete());
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
	auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); CallbackFinished.Trigger();
	}, {}, Options());
	ASSERT_TRUE(Request);
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(Started.WaitFor(2.0)); Fixture.Service->Close();
	EXPECT_FALSE(Request->IsComplete()); Release.Trigger();
	EXPECT_EQ(Fixture.Service->Drain(), EBuildDrainResult::Drained);
	EXPECT_TRUE(CallbackFinished.WaitFor(0.0)); EXPECT_TRUE(Request->IsComplete());
	Worker.join();
}

TEST(FBuildSessionTests, ServiceDrainTracksStateAfterLastSessionHandleDiesInCallback)
{
	FFixture Fixture;
	FThreadEvent CallbackStarted, ReleaseCallback, DrainReturned;
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> {
		Queued = std::move(Work); return {};
	});
	auto Request = Session->Build(Definition(), [&](FBuildCompleteParams Result) {
		EXPECT_EQ(Result.GetStatus(), EStatus::Ok);
		Session.reset();
		CallbackStarted.Trigger();
		EXPECT_TRUE(ReleaseCallback.WaitFor(2.0));
	}, {}, Options());
	ASSERT_TRUE(Request);
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
	EXPECT_TRUE(Request->IsComplete());
}

TEST(FBuildSessionTests, AdmissionErrorsNeverInvokeCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	auto MissingCallback = Session->Build(Definition(), {}, {}, Options());
	ASSERT_FALSE(MissingCallback); EXPECT_EQ(MissingCallback.error().Reason, EBuildAdmissionReason::InvalidRequest);
	auto MissingFunction = Session->Build(Definition("Missing.Function"), [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(MissingFunction); EXPECT_EQ(MissingFunction.error().Reason, EBuildAdmissionReason::MissingFunction);
	FBuildFunctionDescriptor Mismatch{
		.Name = "Session.Fixture",
		.Version = 1,
		.ConstantsSchema = 1,
		.OutputType = "Fixture.Output",
		.OutputSchema = 1,
		.Bucket = FCacheBucket::FromString("Sessions")};
	++Mismatch.Version;
	auto InvalidAction = std::move(FBuildActionBuilder(Definition(), std::move(Mismatch))).Build().value();
	auto InvalidRequest = Session->Build(std::move(InvalidAction), [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(InvalidRequest); EXPECT_EQ(InvalidRequest.error().Reason, EBuildAdmissionReason::InvalidRequest);
	Session->Close();
	auto Closed = Session->Build(Definition(), [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(Closed); EXPECT_EQ(Closed.error().Reason, EBuildAdmissionReason::Closed);
	EXPECT_EQ(Calls, 0u);
}

TEST(FBuildSessionTests, CallbackExceptionsDoNotBreakCompletionOrDrainAccounting)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	auto Request = Session->Build(Definition(), [](FBuildCompleteParams) { throw std::runtime_error("callback"); },
		{}, Options());
	ASSERT_TRUE(Request);
	EXPECT_TRUE(Request->IsComplete());
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
		auto Request = Session->Build(Definition(), Owner, [&](auto) {
			CallbackStarted.Trigger();
			EXPECT_TRUE(ReleaseCallback.WaitFor(2.0));
		}, {}, Options());
		ASSERT_TRUE(Request);
		EXPECT_EQ(Request->Wait(), ERequestWaitResult::Completed);
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
	ASSERT_TRUE(Session->Build(Definition(), [](auto) {}, {}, Options()));
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
	auto Request = First->Build(Definition(), Owner, Completed, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_FALSE(Owner.Poll());
	Owner.Cancel();
	ASSERT_TRUE(Second->Build(Definition(), Owner, Completed, {}, Options()));
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
		EXPECT_FALSE(Session->Build(Definition("Missing"), Owner, [](auto) {}, {}, Options()));
		EXPECT_TRUE(Owner.Poll());
		ASSERT_TRUE(Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
		}, {}, Options()));
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
	FBuildRequest Handle;
	auto Request = Session->Build(Definition(), Owner, [&](auto) {
		EXPECT_EQ(Handle.Wait(), ERequestWaitResult::WouldBlock);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::WouldBlock);
	}, {}, Options());
	ASSERT_TRUE(Request); Handle = *Request;
	Queued();
	EXPECT_EQ(Handle.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, TaskSchedulerWaitHelpsNestedBuildWithOneWorker)
{
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), CreateTaskCacheScheduler()).value();
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("NestedDerivedData", [&] {
		FRequestOwner Owner(ERequestPriority::High);
		auto Request = Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls;
		}, {}, Options());
		ASSERT_TRUE(Request);
		EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High);
		EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
		EXPECT_TRUE(Request->IsComplete());
	});
	EXPECT_EQ(Parent.Wait().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
}

TEST(FBuildSessionTests, TaskSchedulerRejectsWhenCoreIsStoppedWithoutCallback)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), CreateTaskCacheScheduler()).value();
	FRequestOwner Owner;
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), Owner, [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(Request);
	EXPECT_EQ(Request.error().Reason, EBuildAdmissionReason::DispatchRejected);
	EXPECT_EQ(Calls, 0u);
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
	auto Policy = Options(); Policy.Policy.MaximumWorkingSetBytes = 12345;
	ASSERT_TRUE(Session->Build(Definition(), Owner, [](auto) {}, {}, std::move(Policy)));
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
}

TEST(FBuildSessionTests, CanceledOwnerDoesNotScheduleNewWork)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler(), CreateTaskCacheScheduler()).value();
	FRequestOwner Owner; Owner.Cancel();
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
	}, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, InvalidRequestWaitDoesNotReportCompletion)
{
	FBuildRequest Request;
	EXPECT_FALSE(Request.IsComplete());
	EXPECT_EQ(Request.Wait(), ERequestWaitResult::InvalidRequest);
}

TEST(FBuildSessionTests, OwnerMayRequestCancellationInsideItsCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	auto Owner = std::make_unique<FRequestOwner>();
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), *Owner, [&](auto) {
		++Calls; Owner->RequestCancel();
	}, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Request->Wait(), ERequestWaitResult::Completed);
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
	auto Cache = std::make_shared<FContinuationCache>(); auto Service = CreateBuild({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession().value(); FRequestOwner Owner; uint32 Calls = 0;
	auto Outer = Session->Build(Definition(), Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.GetStatus(), EStatus::Ok); ++Calls;
		auto Inner = Session->Build(Definition(), Owner, [&](auto Result) { EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls; });
		ASSERT_TRUE(Inner); EXPECT_TRUE(Inner->IsComplete()); EXPECT_EQ(Inner->Wait(), ERequestWaitResult::Completed);
		EXPECT_EQ(Calls, 2u);
	});
	ASSERT_TRUE(Outer); EXPECT_EQ(Owner.Wait(), ERequestWaitResult::Completed); EXPECT_EQ(Calls, 2u);
}

TEST(FBuildContinuationTests, CancelRetiresQueuedContinuationWithoutProducerProgress)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({Cache, CreateAsyncCache(Cache, IO)}); auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession({}, Producer).value(); FRequestOwner Owner; uint32 Calls = 0;
	auto Request = Session->Build(Definition(), Owner, [&](auto Completion) { EXPECT_EQ(Completion.GetStatus(), EStatus::Canceled); ++Calls; });
	ASSERT_TRUE(Request); Producer->Run(); IO->Run(); ASSERT_EQ(Producer->Work.size(), 1u);
	Owner.Cancel(); EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_TRUE(Request->IsComplete());
	Producer->Run(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Function->Builds, 0u);
}

TEST(FBuildSessionTests, SynchronousCancelWaitsForRunningCallbackAndCaptureRelease)
{
	FFixture Fixture; std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> std::expected<void, FBuildAdmissionError> { Queued = std::move(Work); return {}; });
	FThreadEvent Entered, Release, CancelStarted, CancelReturned;
	auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
	auto Request = Session->Build(Definition(), [Capture, &Entered, &Release](auto) { Entered.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0)); }, {}, Options());
	ASSERT_TRUE(Request); Capture.reset(); std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(Entered.WaitFor(5.0));
	std::jthread Canceler([&] { CancelStarted.Trigger(); Request->Cancel(); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(Request->IsComplete()); CancelReturned.Trigger(); });
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

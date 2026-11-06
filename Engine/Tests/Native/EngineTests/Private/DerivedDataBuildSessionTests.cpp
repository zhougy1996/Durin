#include "BuildServiceBackendPrivate.h"
#include "BuildCacheRecordPrivate.h"
#include "RequestOwnerAccess.h"
#include "CacheBackend.h"
#include "DerivedDataBuildSession.h"

#include "CacheDiagnosticsPrivate.h"
#include "Threading/ThreadEvent.h"
#include "Threading/Task.h"
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
	auto Options() -> FBuildPolicy
	{ return EBuildPolicy::Build; }
	using FDispatcher = std::function<void(std::function<void()>)>;
	class FTestScheduler final : public IBuildScheduler
	{
	public:
		FDispatcher Dispatcher;
		explicit FTestScheduler(FDispatcher Value) : Dispatcher(std::move(Value)) {}
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
			-> std::shared_ptr<IBuildScheduledWork> override
		{
			Dispatcher(std::move(Work));
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	struct FFixture
	{
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		std::shared_ptr<IBuild> Service = Durin::DerivedData::Private::CreateBuildForBackend();
		FFixture() { Service->Register(Function).value(); }
		auto Session(FDispatcher Dispatcher = {}) -> std::shared_ptr<FBuildSession>
		{ return std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Dispatcher ? std::make_shared<FTestScheduler>(std::move(Dispatcher)) : nullptr)); }
	};
}

namespace
{
	class FStageScheduler final : public IBuildScheduler
	{
	public:
		std::deque<std::function<void()>> Work;
		std::vector<ERequestPriority> Priorities;
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Task)
			-> std::shared_ptr<IBuildScheduledWork> override
		{
			Priorities.push_back(Params.Priority);
			Work.push_back(std::move(Task)); return std::shared_ptr<IBuildScheduledWork>{};
		}
		auto Run() -> void { while (!Work.empty()) { auto Task = std::move(Work.front()); Work.pop_front(); Task(); } }
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
			return Request.Record;
		}
	};
}

TEST(FBuildContinuationTests, SubmissionBarriersRetainNestedInlineCallbacksAndPriority)
{
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend();
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateInlineBuildScheduler()));
	FRequestOwner Owner(ERequestPriority::Low); uint32 Callbacks = 0;
	{
		FRequestBarrier Submission(Owner);
		EXPECT_FALSE(Owner.Poll());
		Owner.SetPriority(ERequestPriority::High);
		Function->OnBuild = [&] { EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High); };
		Session->Build(Definition(), {}, Options(), Owner, [&](auto Result) {
			EXPECT_EQ(Result.Status, EStatus::Ok); ++Callbacks;
			FRequestBarrier Nested(Owner);
			Session->Build(Definition(), {}, Options(), Owner, [&](auto Other) {
				EXPECT_EQ(Other.Status, EStatus::Ok); ++Callbacks;

			}); });
		EXPECT_EQ(Callbacks, 2u);
		EXPECT_FALSE(Owner.Poll());
	}
	EXPECT_TRUE(Owner.Poll()); Owner.Wait();
}

TEST(FBuildContinuationTests, CancellationCannotRetireUnpublishedSubmission)
{
	FRequestOwner Owner; FThreadEvent Entered, Release, Waiting, Retired;
	std::thread Submitter([&] { FRequestBarrier Submission(Owner); Entered.Trigger(); Release.Wait(); });
	Entered.Wait(); Durin::DerivedData::Private::FOwnerAccess::RequestCancel(Owner); EXPECT_FALSE(Owner.Poll());
	std::thread Waiter([&] { Waiting.Trigger(); Owner.Wait(); Retired.Trigger(); });
	Waiting.Wait(); EXPECT_FALSE(Retired.WaitFor(0.02));
	Release.Trigger(); Submitter.join(); Waiter.join(); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, TaskSessionsDoNotChangeInlineSessions)
{
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto ExistingInline = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateInlineBuildScheduler()));
	auto TaskSession = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	auto LaterInline = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateInlineBuildScheduler()));
	const auto Caller = std::this_thread::get_id(); uint32 Callbacks = 0;
	Function->OnBuild = [&] { EXPECT_EQ(std::this_thread::get_id(), Caller); };
	for (const auto& Session : {ExistingInline, LaterInline})
	{
		FRequestOwner RequestOwner;
		Session->Build(Definition(), {}, {}, RequestOwner, [&](auto Completion) {
			EXPECT_EQ(std::this_thread::get_id(), Caller);
			EXPECT_EQ(Completion.Status, EStatus::Ok);
			EXPECT_TRUE(Completion.Output.IsValid());
			++Callbacks;
		});
		RequestOwner.Wait();
	}
	EXPECT_EQ(Callbacks, 2u); EXPECT_EQ(Cache->Gets, 2u); EXPECT_EQ(Cache->Puts, 2u);

}

TEST(FBuildContinuationTests, ServiceRetainedByteAdmissionPreservesLocalOutputAndRecoversAfterRetirement)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO, {.Concurrency = 1, .MaximumRetainedBytes = 14000})});
	auto Function = std::make_shared<FFunction>(); Function->DataBytes = 8192; ASSERT_TRUE(Service->Register(Function));
	auto First = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, {}));
	auto Second = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, {})); uint32 Calls = 0;
	const auto Complete = [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ASSERT_TRUE(Result.Output.IsValid()); ++Calls; };
	FBuildPolicy OptionsPolicy = EBuildPolicy::Build | EBuildPolicy::CacheStore;
	FRequestOwner RequestOwner3;
	First->Build(Definition(), {}, OptionsPolicy, RequestOwner3, Complete);
	EXPECT_EQ(IO->Work.size(), 1u);
	FRequestOwner OverflowOwner;
	Second->Build(Definition(), {}, OptionsPolicy, OverflowOwner, Complete);
	EXPECT_TRUE(OverflowOwner.Poll());
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(IO->Work.size(), 1u);
	IO->Run(); EXPECT_EQ(Calls, 2u);
	FRequestOwner RequestOwner1;
	Second->Build(Definition(), {}, OptionsPolicy, RequestOwner1, Complete);
	EXPECT_EQ(IO->Work.size(), 1u);
	IO->Run(); EXPECT_EQ(Calls, 3u); EXPECT_EQ(Cache->Puts, 2u);
}

TEST(FBuildContinuationTests, ServiceDoesNotCloseInjectedCacheUsedByOtherOwners)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Async = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO);
	auto Service = CreateBuild({.Cache = Async});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
	FRequestOwner RequestOwner1;
	Session->Build(Definition(), {}, {}, RequestOwner1, [](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); });

	FRequestOwner External; uint32 Calls = 0;
	const std::array Requests{FCacheGetRequest{.Key = FCacheKey::FromHash(FCacheBucket::FromString("External"), FXxHash128::HashBuffer("key"))}};
	Async->Get(Requests, External, [&](auto Response) { EXPECT_EQ(Response.Status, EStatus::Error); ++Calls; });
	while (!IO->Work.empty()) IO->Run();
	EXPECT_EQ(Calls, 1u); External.Wait();
}

TEST(FBuildContinuationTests, SharedTaskDispatcherCompletesAcrossSessionsWithOneCoreWorker)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache, .CacheDispatchLimits = {.Concurrency = 1}});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto First = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	auto Second = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("SharedCacheSingleWorker", [&] {
		FRequestOwner Owner;
		const auto Complete = [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; };
		First->Build(Definition(), {}, {}, Owner, Complete);
		Second->Build(Definition(), {}, {}, Owner, Complete);
		Owner.Wait();
	});
	EXPECT_EQ(Parent.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 2u); EXPECT_EQ(Cache->Gets, 2u);

}

TEST(FBuildContinuationTests, IOReleasesProducerQueueAndStoreFailurePreservesLocalOutput)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>(); auto Async = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO, {.Concurrency = 1});
	auto Service = CreateBuild({.Cache = Async});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function)); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer));
	FRequestOwner Owner(ERequestPriority::High); uint32 FirstCallbacks = 0, OtherCallbacks = 0;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.Status, EStatus::Ok);
		EXPECT_TRUE(Completion.Output.IsValid());
		EXPECT_TRUE(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::BuildLocal));

		++FirstCallbacks;
	});
	Producer->Run();
	EXPECT_EQ(Function->Builds, 0u);
	EXPECT_EQ(IO->Work.size(), 1u);
	FRequestOwner OtherOwner;
	Session->Build(Definition(), {}, Options(), OtherOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); ++OtherCallbacks; });
	while (!Producer->Work.empty())
		Producer->Run();
	EXPECT_EQ(OtherCallbacks, 1u);
	EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run(); EXPECT_EQ(Producer->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	Producer->Run(); EXPECT_EQ(Function->Builds, 2u); EXPECT_EQ(IO->Work.size(), 1u); EXPECT_EQ(FirstCallbacks, 0u);
	IO->Run();
	EXPECT_EQ(FirstCallbacks, 1u);
	Owner.Wait();
	EXPECT_EQ(Cache->Gets, 1u); EXPECT_EQ(Cache->Puts, 1u);
	Cache->ThrowPut = true;
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, Owner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); EXPECT_TRUE(Completion.Output.IsValid()); ++FirstCallbacks; });
	Producer->Run();
	IO->Run();
	EXPECT_EQ(FirstCallbacks, 2u);

	for (auto Priority : IO->Priorities) EXPECT_EQ(Priority, ERequestPriority::High);
}

TEST(FBuildContinuationTests, FullHitsSkipProducerAndPartialHitsRebuildOnce)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->Hit = true;
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer)); uint32 Callbacks = 0;
	FRequestOwner HitOwner;
	Session->Build(Definition(), {}, {}, HitOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::CacheQueryHit)); ++Callbacks; });
	Producer->Run();
	IO->Run();
	EXPECT_EQ(Function->Builds, 0u);
	EXPECT_EQ(Callbacks, 1u);
	EXPECT_TRUE(Producer->Work.empty());
	Cache->Partial = true;
	FRequestOwner PartialOwner;
	Session->Build(Definition(), {}, {}, PartialOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); EXPECT_TRUE(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::BuildLocal)); ++Callbacks; });
	Producer->Run();
	IO->Run();
	Producer->Run();
	IO->Run();
	EXPECT_EQ(Callbacks, 2u);
	EXPECT_EQ(Function->Builds, 1u);
	PartialOwner.Wait();
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
		ASSERT_TRUE(Service->Register(Function)); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
		uint32 Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), {}, BuildLocal ? EBuildPolicy::Default : EBuildPolicy::Cache, RequestOwner, [&](auto Completion) {
			++Calls;
			EXPECT_FALSE(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::CacheQueryHit));
			EXPECT_EQ(Completion.Status, BuildLocal ? EStatus::Ok : EStatus::Error);
			EXPECT_EQ(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::BuildLocal), BuildLocal);
			if (BuildLocal)
			{
				ASSERT_TRUE(Completion.Output.IsValid());
				EXPECT_EQ(Completion.Output.GetSchema(), "Fixture.Output");
				EXPECT_EQ(Completion.Output.GetSchemaVersion(), 1u);
			}
			else
				EXPECT_TRUE(Completion.Output.IsValid());
		});
		RequestOwner.Wait();
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
		ASSERT_TRUE(Service->Register(Function)); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture")); uint32 Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), {}, BuildLocal ? EBuildPolicy::Default : EBuildPolicy::Cache, RequestOwner, [&](auto Completion) {
			++Calls;
			EXPECT_EQ(Completion.Status, BuildLocal ? EStatus::Ok : EStatus::Error);
			if (BuildLocal)
			{
				ASSERT_TRUE(Completion.Output.IsValid());
				EXPECT_FALSE(Completion.Output.HasError());
			}
			else
				EXPECT_TRUE(Completion.Output.IsValid());
			EXPECT_FALSE(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::CacheQueryHit));
			EXPECT_EQ(HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::BuildLocal), BuildLocal);
		});
		RequestOwner.Wait();
		EXPECT_EQ(Calls, 1u); EXPECT_EQ(Function->Builds, BuildLocal ? 1u : 0u); EXPECT_EQ(Cache->Puts, BuildLocal ? 1u : 0u);
	}
}

TEST(FBuildContinuationTests, SessionResolverStaysAliveThroughBuildCompletion)
{
	class FLeaseResolver final : public IBuildInputResolver
	{
	public:
		std::shared_ptr<int> Lease;
		auto ResolveInputMeta(const FBuildDefinition& Definition, IRequestOwner& Owner, FOnBuildInputMetaResolved&& OnResolved) const -> void override
			{
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled}); return OnResolved({std::vector<FBuildInputReference>{}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok}); }
		auto ResolveInputData(const FBuildAction& Action, IRequestOwner& Owner, FOnBuildInputDataResolved&& OnResolved) const -> void override
			{
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled}); return OnResolved({std::vector<FBuildInput>{}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok}); }
	};
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer)); auto Resolver = std::make_shared<FLeaseResolver>(); Resolver->Lease = std::make_shared<int>(7);
	std::weak_ptr<int> Lease = Resolver->Lease; uint32 Callbacks = 0;
	FRequestOwner RequestOwner;
	Service->CreateSession("Fixture", Resolver, Producer).Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); EXPECT_FALSE(Lease.expired()); ++Callbacks; });
	Resolver.reset();
	Producer->Run(); EXPECT_FALSE(Lease.expired());
	EXPECT_EQ(Callbacks, 0u); IO->Run(); RequestOwner.Wait(); EXPECT_EQ(Callbacks, 1u); EXPECT_TRUE(Lease.expired());
}

TEST(FBuildContinuationTests, OwnerWaitHelpsResumeWhenEarlierDispatchReturnsLater)
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
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work) -> std::shared_ptr<IBuildScheduledWork> override
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
		auto Schedule(const FBuildScheduleParams&, std::function<void()> Work) -> std::shared_ptr<IBuildScheduledWork> override
		{
			Thread = std::thread(std::move(Work)); ResumeEntered.wait();
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	std::promise<void> Release; auto Producer = std::make_shared<FDelayedProducer>(); Producer->Release = Release.get_future().share();
	auto IO = std::make_shared<FConcurrentIO>(); IO->ResumeEntered = Producer->Entered.get_future().share();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer));
	std::atomic<uint32> Callbacks = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheQuery, RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); ++Callbacks; });
	Release.set_value();
	auto Wait = std::async(std::launch::async, [&] { return RequestOwner.Wait(); });
	const auto Status = Wait.wait_for(std::chrono::seconds(5));
	// On regression, release the queued producer manually so diagnostics can finish.
	if (Status != std::future_status::ready) Producer->Resume->Wait();
	EXPECT_EQ(Status, std::future_status::ready); Wait.get(); EXPECT_EQ(Callbacks, 1u);
	IO->Thread.join();
}

TEST(FBuildContinuationTests, SlowCoreIOLeavesSingleWorkerAvailableAndOwnerWaitsForCallback)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>(); std::promise<void> Entered, Release, WorkerProgress;
	auto Released = Release.get_future().share(); auto Started = Entered.get_future(); auto Progress = WorkerProgress.get_future();
	Cache->BeforeGet = [&] { Entered.set_value(); Released.wait(); };
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler())); std::atomic<uint32> Callbacks = 0;
	FRequestOwner RequestOwner;
	Session->Build(Definition(), {}, {}, RequestOwner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Canceled); ++Callbacks; });
	const auto StartStatus = Started.wait_for(std::chrono::seconds(5));
	auto Other = Tasks::LaunchTask("DDCWorkerProgress", [&] { WorkerProgress.set_value(); });
	const auto ProgressStatus = Progress.wait_for(std::chrono::seconds(5));
	Durin::DerivedData::Private::FOwnerAccess::RequestCancel(RequestOwner);

	EXPECT_FALSE(RequestOwner.Poll());
	Release.set_value();
	EXPECT_EQ(StartStatus, std::future_status::ready); EXPECT_EQ(ProgressStatus, std::future_status::ready);
	RequestOwner.Wait();

	EXPECT_EQ(Callbacks, 1u);
	Other.Wait();
}

TEST(FBuildContinuationTests, BuildAndCacheShareOwnerAcrossNestedCallbacks)
{
	auto Producer = std::make_shared<FStageScheduler>();
	auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Async = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO, {.Concurrency = 1});
	auto Service = CreateBuild({.Cache = Async});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer));
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Ok);

		const std::array Reads{FCacheGetRequest{Result.CacheKey}};
		Async->Get(Reads, Owner, [&](auto Response) {

			++Calls;
		});
		++Calls;
	});
	Producer->Run();
	EXPECT_FALSE(Owner.Poll());
	IO->Run();
	Producer->Run();
	IO->Run();
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Calls, 2u);
	Owner.Wait();
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildContinuationTests, ExecutionFailureDoesNotSubmitAsyncPutAndCanRetry)
{
	auto Producer = std::make_shared<FStageScheduler>();
	auto IO = std::make_shared<FStageScheduler>();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Function = std::make_shared<FFunction>();
	Function->FailExecution = true;
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	ASSERT_TRUE(Service->Register(Function));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer));
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Error);
		EXPECT_TRUE(Result.Output.IsValid());
		++Calls;
	});
	Producer->Run(); IO->Run(); Producer->Run();
	EXPECT_EQ(Cache->Puts.load(), 0u);
	EXPECT_TRUE(IO->Work.empty());
	EXPECT_TRUE(Owner.Poll());
	Function->FailExecution = false;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
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
	auto First = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
	ASSERT_TRUE(Service->Register(std::make_shared<FSecondFunction>()));
	auto Second = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
	uint32 Calls = 0;
	std::optional<EStatus> MissingStatus;
	FRequestOwner MissingOwner;
	First->Build(Definition("Session.Second"), {}, Options(), MissingOwner, [&](auto Result) { MissingStatus = Result.Status; ++Calls; });
	ASSERT_EQ(MissingStatus, EStatus::Error);
	FRequestOwner AcceptedOwner;
	Second->Build(Definition("Session.Second"), {}, Options(), AcceptedOwner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.Status, EStatus::Ok); });
	EXPECT_TRUE(AcceptedOwner.Poll());
	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildSessionTests, PersistentSessionCompletesMultipleRequestsAndReentersWithoutLocks)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	FRequestOwner FirstOwner;
	Session->Build(Definition(), {}, Options(), FirstOwner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.Status, EStatus::Ok);
		FRequestOwner NestedOwner;
	Session->Build(Definition(), {}, Options(), NestedOwner, [&](FBuildCompleteParams Inner) {
			++Calls; EXPECT_EQ(Inner.Status, EStatus::Ok);
		});
		EXPECT_TRUE(NestedOwner.Poll());
		 });
	EXPECT_TRUE(FirstOwner.Poll());
	EXPECT_EQ(Calls, 2u);

	EXPECT_EQ(Fixture.Function->Builds.load(), 2u);
}

TEST(FBuildSessionTests, DroppedWorkCompletesOnce)
{
	FFixture Fixture;
	auto Session = Fixture.Session([](auto) {});
	uint32 Calls = 0;
	FRequestOwner Owner;
	Session->Build(Definition(), {}, Options(), Owner, [&](auto Result) {
		++Calls; EXPECT_EQ(Result.Status, EStatus::Error);
	});
	Owner.Wait();
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, DroppedDispatchRacesPublicationCompletesExactlyOnce)
{
	FFixture Fixture;
	std::barrier Start(2);
	std::jthread Dropper;
	auto Session = Fixture.Session([&](auto Work) -> void {
		Dropper = std::jthread([Work = std::move(Work), &Start]() mutable {
			Start.arrive_and_wait();
			Work = {};
		});
		Start.arrive_and_wait();
	});
	for (uint32 Round = 0; Round < 50; ++Round)
	{
		std::atomic<uint32> Calls = 0;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), {}, Options(), RequestOwner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.Status, EStatus::Error);
			++Calls; });
		Dropper.join();
		EXPECT_TRUE(RequestOwner.Poll());
		EXPECT_EQ(Calls.load(), 1u);
	}
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);

}

TEST(FBuildSessionTests, QueuedCancellationAndRunnerRaceCompleteExactlyOnce)
{
	FFixture Fixture;
	std::function<void()> Queued;
	auto Session = Fixture.Session([&](auto Work) -> void {
		Queued = std::move(Work);
	});
	for (uint32 Round = 0; Round < 50; ++Round)
	{
		std::atomic<uint32> Calls = 0;
		std::atomic<bool> Canceled = false;
		FRequestOwner RequestOwner;
		Session->Build(Definition(), {}, Options(), RequestOwner, [&](FBuildCompleteParams Result) {
			Canceled = Result.Status == EStatus::Canceled; ++Calls; });
		std::barrier Start(2);
		std::jthread Worker([&] {
			Start.arrive_and_wait();
			while (Queued) { auto Work = std::exchange(Queued, {}); Work(); }
		});
		Start.arrive_and_wait();
		RequestOwner.Cancel();
		Worker.join();
		EXPECT_TRUE(RequestOwner.Poll());
		EXPECT_EQ(Calls.load(), 1u);
		EXPECT_TRUE(RequestOwner.IsCanceled());
		Queued = {};
	}

}

TEST(FBuildSessionTests, InvalidRequestsCompleteExactlyOnce)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	uint32 Calls = 0;
	std::optional<EStatus> MissingFunctionStatus;
	FRequestOwner MissingFunctionOwner;
	Session->Build(Definition("Missing.Function"), {}, Options(), MissingFunctionOwner, [&](auto Result) { MissingFunctionStatus = Result.Status; ++Calls; });
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
	Session->Build(std::move(InvalidAction), {}, Options(), InvalidRequestOwner, [&](auto Result) { InvalidRequestStatus = Result.Status; ++Calls; });
	ASSERT_EQ(InvalidRequestStatus, EStatus::Error);

	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildSessionTests, CallbackExceptionsDoNotBreakOwnerCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	FRequestOwner RequestOwner;
	Session->Build(Definition(), {}, Options(), RequestOwner, [](FBuildCompleteParams) { throw std::runtime_error("callback"); });
	EXPECT_TRUE(RequestOwner.Poll());

	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
}

TEST(FBuildSessionTests, OwnerWaitIncludesCallbacksAndSessionRemainsReusable)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
	FThreadEvent Entered, Release, Returned;
	Session->Build(Definition(), {}, Options(), Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); Entered.Trigger(); Release.Wait(); });
	ASSERT_TRUE(Entered.WaitFor(5.0));
	std::jthread Waiter([&] { Owner.Wait(); Returned.Trigger(); });
	EXPECT_FALSE(Returned.WaitFor(0.05)); Release.Trigger(); Waiter.join();
	EXPECT_TRUE(Owner.Poll());
	Session->Build(Definition(), {}, Options(), Owner, {}); Owner.Wait();
	EXPECT_EQ(Fixture.Function->Builds, 2u);
}


TEST(FBuildSessionTests, OwnerCancellationCoversExistingAndFutureRequestsAcrossSessions)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	auto First = Fixture.Session([&](auto Work) -> void { Queue.push_back(std::move(Work)); });
	auto Second = Fixture.Session();
	FRequestOwner Owner;
	uint32 Calls = 0;
	auto Completed = [&](FBuildCompleteParams Result) { ++Calls; EXPECT_EQ(Result.Status, EStatus::Canceled); };
	First->Build(Definition(), {}, Options(), Owner, Completed);
	EXPECT_FALSE(Owner.Poll());
	Owner.Cancel();
	Second->Build(Definition(), {}, Options(), Owner, Completed);
	Owner.Wait();
	EXPECT_TRUE(Owner.IsCanceled());
	EXPECT_EQ(Calls, 2u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	Queue.clear();
}

TEST(FBuildSessionTests, OwnerDestructionCancelsQueuedWork)
{
	FFixture Fixture;
	std::function<void()> Stale;
	auto Session = Fixture.Session([&](auto Work) -> void { Stale = std::move(Work); });
	uint32 Calls = 0;
	{
		FRequestOwner Owner;
		Session->Build(Definition(), {}, Options(), Owner, [&](FBuildCompleteParams Result) {
			++Calls; EXPECT_EQ(Result.Status, EStatus::Canceled); });
	}
	EXPECT_EQ(Calls, 1u);
	Stale();
	EXPECT_EQ(Calls, 1u);
}

TEST(FBuildSessionTests, TaskSchedulerWaitHelpsNestedBuildWithOneWorker)
{
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture;
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("NestedDerivedData", [&] {
		FRequestOwner Owner(ERequestPriority::High);
		Session->Build(Definition(), {}, Options(), Owner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
		EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High);
		Owner.Wait();
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
}

TEST(FBuildSessionTests, BootstrapBuildCompletesWithoutCoreScheduler)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	FRequestOwner Owner;
	uint32 Calls = 0;
	std::optional<EStatus> RequestStatus;
	Session->Build(Definition(), {}, Options(), Owner, [&](auto Result) { RequestStatus = Result.Status; ++Calls; });
	ASSERT_EQ(RequestStatus, EStatus::Ok);
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Owner.Poll());
	Owner.Wait();
}

TEST(FBuildSessionTests, SchedulerReceivesOwnerPriority)
{
	class FRecordingScheduler final : public IBuildScheduler
	{
	public:
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::shared_ptr<IBuildScheduledWork> override
		{
			EXPECT_EQ(Params.FunctionName, "Session.Fixture");
			EXPECT_EQ(Params.Priority, ERequestPriority::Low);

			Work();
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	FFixture Fixture;
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, std::make_shared<FRecordingScheduler>()));
	FRequestOwner Owner(ERequestPriority::Low);
	auto Policy = Options();
	Session->Build(Definition(), {}, std::move(Policy), Owner, [](auto) {});
	Owner.Wait();
}

TEST(FBuildSessionTests, ReusedPolicyDoesNotShareCancellationAcrossOwners)
{
	FFixture Fixture;
	auto Scheduler = std::make_shared<FStageScheduler>();
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, Scheduler));
	FRequestOwner First, Second;
	const auto SharedOptions = Options();
	uint32 Canceled = 0, Completed = 0;
	Fixture.Function->OnBuild = [&] { Durin::DerivedData::Private::FOwnerAccess::RequestCancel(First); };
	Session->Build(Definition(), {}, SharedOptions, First, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Canceled);
		EXPECT_TRUE(Result.Output.IsValid());
		++Canceled;
	});
	Scheduler->Run();
	First.Wait();
	EXPECT_FALSE(Second.IsCanceled());
	Session->Build(Definition(), {}, SharedOptions, Second, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Ok);
		EXPECT_TRUE(Result.Output.IsValid());
		++Completed;
	});
	Scheduler->Run();
	Second.Wait();
	Session->Build(Definition(), {}, SharedOptions, First, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Canceled);
		++Canceled;
	});
	First.Wait();
	EXPECT_EQ(Canceled, 2u);
	EXPECT_EQ(Completed, 1u);
	EXPECT_EQ(Fixture.Function->Builds, 2u);
}

TEST(FBuildSessionTests, UnsupportedPolicyCompletesOnceWithoutDispatch)
{
	FFixture Fixture;
	auto Scheduler = std::make_shared<FStageScheduler>();
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, Scheduler));
	FRequestOwner Owner;
	uint32 Calls = 0;
	Session->Build(Definition(), {}, static_cast<EBuildPolicy>(1 << 7), Owner, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Error);
		EXPECT_FALSE((Result.Output.GetLogs().empty() ? std::string_view{} : std::string_view(Result.Output.GetLogs().front().Text)).empty());
		++Calls;
	});
	Owner.Wait();
	EXPECT_EQ(Calls, 1u);
	EXPECT_TRUE(Scheduler->Work.empty());
	EXPECT_EQ(Fixture.Function->Builds, 0u);
}

TEST(FBuildSessionTests, CanceledOwnerDoesNotScheduleNewWork)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	FRequestOwner Owner; Owner.Cancel();
	uint32 Calls = 0;
	Session->Build(Definition(), {}, Options(), Owner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.Status, EStatus::Canceled); });
	Owner.Wait();
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, EmptyOwnerIsAlreadyComplete)
{
	FRequestOwner Owner;
	EXPECT_TRUE(Owner.Poll());
	Owner.Wait();
}

TEST(FBuildSessionTests, OwnerMayRequestCancellationInsideItsCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	auto Owner = std::make_unique<FRequestOwner>();
	uint32 Calls = 0;
	Session->Build(Definition(), {}, Options(), *Owner, [&](auto) {
		++Calls; Durin::DerivedData::Private::FOwnerAccess::RequestCancel(*Owner); });
	EXPECT_EQ(Calls, 1u);
	(*Owner).Wait();

}

TEST(FBuildSessionDeathTests, OwnerDestructionInsideCompletionIsRejected)
{
	EXPECT_DEATH({
		FFixture Fixture; auto Session = Fixture.Session(); auto Owner = std::make_unique<FRequestOwner>();
		(void)Session->Build(Definition(), {}, Options(), *Owner, [&](auto) { Owner.reset(); });
	}, "outside their callbacks");
}

TEST(FBuildContinuationTests, InlineNestedBuildCompletesBeforeOuterCallbackReturns)
{
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture")); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Completion) {
		EXPECT_EQ(Completion.Status, EStatus::Ok);
		++Calls;
		Session->Build(Definition(), {}, {}, Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
		EXPECT_FALSE(Owner.Poll());

		EXPECT_EQ(Calls, 2u);
	});
	Owner.Wait();
	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildContinuationTests, CancelRetiresQueuedContinuationWithoutProducerProgress)
{
	auto Producer = std::make_shared<FStageScheduler>(); auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	auto Function = std::make_shared<FFunction>();
	ASSERT_TRUE(Service->Register(Function));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer)); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), {}, {}, Owner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Canceled); ++Calls; });
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
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
	FThreadEvent Entered, Release, CancelStarted, Returned;
	auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
	Session->Build(Definition(), {}, Options(), Owner, [Capture, &Entered, &Release](auto) { Entered.Trigger(); Release.Wait(); });
	Capture.reset(); ASSERT_TRUE(Entered.WaitFor(5.0));
	std::jthread Canceler([&] { CancelStarted.Trigger(); Owner.Cancel(); Returned.Trigger(); });
	CancelStarted.Wait(); EXPECT_FALSE(Returned.WaitFor(0.05)); Release.Trigger(); Canceler.join();
	EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(Owner.Poll());
}


TEST(FBuildSessionDeathTests, SynchronousOwnerCancellationInsideCallbackIsRejected)
{
	EXPECT_DEATH({
		FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
		(void)Session->Build(Definition(), {}, Options(), Owner, [&](auto) { Owner.Cancel(); });
	}, "outside their callbacks");
}

TEST(FBuildContinuationTests, CompletionWaitsForPersistenceAndRetiresCaptures)
{
	for (uint32 StoreMode = 0; StoreMode < 3; ++StoreMode)
	{
		SCOPED_TRACE(StoreMode);
		auto Producer = std::make_shared<FStageScheduler>(), IO = std::make_shared<FStageScheduler>();
		auto Cache = std::make_shared<FContinuationCache>();
		Cache->FailPut = StoreMode == 1; Cache->ThrowPut = StoreMode == 2;
		auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
		auto Function = std::make_shared<FFunction>();
		ASSERT_TRUE(Service->Register(Function)); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, Producer));
		FRequestOwner Owner; uint32 Calls = 0; std::optional<FBuildOutput> Delivered;
		auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
		Cache->BeforePut = [&] { EXPECT_EQ(Calls, 0u); EXPECT_FALSE(Weak.expired()); };
		Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, Owner, [&, Capture](auto Completion) {
			++Calls;
			EXPECT_EQ(Cache->Puts, 1u);
			EXPECT_EQ(Completion.Status, EStatus::Ok);
			ASSERT_TRUE(Completion.Output.IsValid());
			Delivered = Completion.Output;
			EXPECT_FALSE(Weak.expired());

		});
		Capture.reset(); Producer->Run();
		EXPECT_EQ(Calls, 0u); EXPECT_FALSE(Weak.expired()); EXPECT_FALSE(Owner.Poll());
		ASSERT_EQ(IO->Work.size(), 1u); EXPECT_EQ(Cache->Puts, 0u);
		FThreadEvent Waiting, Retired;
		std::jthread Waiter([&] { Waiting.Trigger(); Owner.Wait(); Retired.Trigger(); });
		Waiting.Wait(); EXPECT_FALSE(Retired.WaitFor(0.02));
		IO->Run(); Waiter.join();
		EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(Owner.Poll());
		ASSERT_TRUE(Delivered); EXPECT_FALSE(Delivered->HasError());
		ASSERT_NE(Delivered->FindValue(FValueId::FromName("Data")), nullptr);
		EXPECT_EQ(Delivered->FindValue(FValueId::FromName("Data"))->GetRawSize(), 32u);

	}
}

TEST(FBuildContinuationTests, CancellationWhileStoreQueuedDiscardsLocalResultOnce)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>();
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture")); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, Owner, [&](auto Completion) {
		++Calls; EXPECT_EQ(Completion.Status, EStatus::Canceled); EXPECT_TRUE(Completion.Output.IsValid());
	});
	EXPECT_EQ(Function->Builds, 1u); EXPECT_EQ(Calls, 0u); ASSERT_EQ(IO->Work.size(), 1u);
	Owner.Cancel();
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Cache->Puts, 0u);
	IO->Run(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Cache->Puts, 0u);

}

TEST(FBuildContinuationTests, CancellationWhileStoreRunningWaitsForStoreRetirement)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Cache = std::make_shared<FContinuationCache>(); Cache->FailPut = false;
	auto Service = CreateBuild({.Cache = Durin::DerivedData::Private::CreateCacheForBackend(Cache, IO)});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
	FThreadEvent Entered, Release; FRequestOwner Owner; std::atomic<uint32> Calls = 0;
	auto Capture = std::make_shared<int>(1); std::weak_ptr<int> Weak = Capture;
	Cache->BeforePut = [&] { Entered.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0)); };
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, Owner, [&, Capture](auto Completion) {
		EXPECT_EQ(Completion.Status, EStatus::Canceled); EXPECT_TRUE(Completion.Output.IsValid()); ++Calls;
	});
	Capture.reset(); EXPECT_EQ(Calls.load(), 0u); ASSERT_EQ(IO->Work.size(), 1u);
	std::jthread Worker([&] { IO->Run(); });
	const bool Started = Entered.WaitFor(5.0);
	EXPECT_TRUE(Started);
	Durin::DerivedData::Private::FOwnerAccess::RequestCancel(Owner);
	EXPECT_EQ(Calls.load(), 0u); EXPECT_FALSE(Owner.Poll()); EXPECT_FALSE(Weak.expired());
	Release.Trigger(); Worker.join();
	Owner.Wait();
	EXPECT_EQ(Calls.load(), 1u); EXPECT_EQ(Cache->Puts, 1u); EXPECT_TRUE(Weak.expired());

}

TEST(FBuildContinuationTests, CompletionAndPersistenceDrainRemainLiveWithOneWorker)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture", {}, CreateTaskBuildScheduler()));
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("BuildCompletionSingleWorker", [&] {
		FRequestOwner Owner;
		std::optional<EStatus> RequestStatus;
		Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheStore, Owner, [&](auto Result) {
			RequestStatus = Result.Status;
			EXPECT_EQ(Result.Status, EStatus::Ok);
			++Calls;
		});
		Owner.Wait();
		EXPECT_TRUE(RequestStatus.has_value());
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls.load(), 1u); EXPECT_EQ(Cache->Puts, 1u);
}

TEST(FBuildContinuationTests, DiagnosticsCountRebuildFallbackAndSchemaRejectionWithoutChangingCompletion)
{
	Durin::DerivedData::Private::ResetCacheDiagnosticsForTests();
	auto Cache = std::make_shared<FContinuationCache>();
	auto Service = Durin::DerivedData::Private::CreateBuildForBackend({.Cache = Cache});
	ASSERT_TRUE(Service->Register(std::make_shared<FFunction>())); auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture"));
	uint32 Calls = 0;
	FRequestOwner RequestOwner3;
	Session->Build(Definition(), {}, {}, RequestOwner3, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
	Cache->Hit = true; Cache->OutputSchema = "Wrong.Schema";
	FRequestOwner RequestOwner2;
	Session->Build(Definition(), {}, {}, RequestOwner2, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
	FRequestOwner RequestOwner1;
	Session->Build(Definition(), {}, EBuildPolicy::Cache, RequestOwner1, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Error); ++Calls; });
	const auto Stats = GetCacheDiagnostics();
	EXPECT_EQ(Stats.Total.BuildFallbacks, 2u); EXPECT_EQ(Stats.Total.SchemaRejections, 2u); EXPECT_EQ(Calls, 3u);
}

TEST(FBuildSessionTests, EmptyCompletionIsSupported)
{
	FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner;
	Session->Build(Definition(), {}, Options(), Owner, {});
	Owner.Wait(); EXPECT_TRUE(Owner.Poll()); EXPECT_EQ(Fixture.Function->Builds, 1u);
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
		Owner.LaunchTask("OwnerPublicLaunchChild", [&] { ++Calls;  });
		Owner.Wait();
		EXPECT_TRUE(Owner.Poll());
	});
	EXPECT_EQ(Parent.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_EQ(Calls, 1u);
}

TEST(FRequestOwnerTests, PublicLaunchStillInvokesBodyForCanceledOwnerAndStoppedScheduler)
{
	ShutdownTaskScheduler(false);
	FRequestOwner Owner;
	uint32 Calls = 0;
	Owner.LaunchTask("StoppedOwnerTask", [&] { ++Calls;  });
	Owner.Cancel();
	Owner.LaunchTask("CanceledOwnerTask", [&] { ++Calls; EXPECT_TRUE(Owner.IsCanceled()); });
	Owner.Wait();
	EXPECT_EQ(Calls, 2u);
}

TEST(FBuildContinuationTests, PublicCacheStatusControlsFallbackAndPreservesLocalOutputOnStoreError)
{
	class FStatusCache final : public ICache
	{
	public:
		EStatus GetStatus = EStatus::Error, PutStatus = EStatus::Error;
		uint32 Gets = 0, Puts = 0;
		auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner&, FOnCacheGetComplete&& Complete) -> void override
		{ for (const auto& Request : Requests) { ++Gets; Complete(Request.MakeResponse(GetStatus)); } }
		auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner&, FOnCachePutComplete&& Complete) -> void override
		{ for (const auto& Request : Requests) { ++Puts; Complete(Request.MakeResponse(PutStatus)); } }
	};
	for (const auto GetStatus : {EStatus::Error, EStatus::Canceled}) for (const auto PutStatus : {EStatus::Ok, EStatus::Error, EStatus::Canceled}) {
		auto Cache = std::make_shared<FStatusCache>(); Cache->GetStatus = GetStatus; Cache->PutStatus = PutStatus;
		auto Service = CreateBuild({.Cache = Cache}); auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
		auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture")); FRequestOwner Owner; uint32 Calls = 0;
		Session->Build(Definition(), {}, {}, Owner, [&](auto Result) {
			const bool Canceled = GetStatus == EStatus::Canceled || PutStatus == EStatus::Canceled;
			EXPECT_EQ(Result.Status, Canceled ? EStatus::Canceled : EStatus::Ok);
			EXPECT_TRUE(Result.Output.IsValid()); ++Calls;
		});
		Owner.Wait(); EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Function->Builds, GetStatus == EStatus::Canceled ? 0u : 1u);
		EXPECT_EQ(Cache->Gets, 1u); EXPECT_EQ(Cache->Puts, GetStatus == EStatus::Canceled ? 0u : 1u);
	}
}

TEST(FBuildContinuationTests, ParentOwnerPriorityReachesQueuedCacheRequests)
{
	auto IO = std::make_shared<FStageScheduler>(); auto Backend = std::make_shared<FContinuationCache>();
	auto Cache = Durin::DerivedData::Private::CreateCacheForBackend(Backend, IO, {.Concurrency = 1});
	auto Service = CreateBuild({.Cache = Cache}); ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Service->CreateSession("Fixture")); FRequestOwner Busy, Owner(ERequestPriority::Low); uint32 Calls = 0;
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheQuery, Busy, [](auto) {});
	Session->Build(Definition(), {}, EBuildPolicy::Build | EBuildPolicy::CacheQuery, Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
	Owner.SetPriority(ERequestPriority::High);
	while (!IO->Work.empty()) IO->Run();
	ASSERT_EQ(IO->Priorities.size(), 2u); EXPECT_EQ(IO->Priorities.back(), ERequestPriority::High);
	EXPECT_EQ(Calls, 1u); Owner.Wait(); Busy.Wait();
}

TEST(FBuildSessionTests, MoveOnlyCompletionRunsOnceAndSessionHasName)
{
	FFixture Fixture; auto Session = std::make_shared<Durin::DerivedData::FBuildSession>(Fixture.Service->CreateSession("MoveOnlyBuild"));
	EXPECT_EQ(Session->GetName(), "MoveOnlyBuild"); FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), {}, Options(), Owner, [State = std::make_unique<uint32>(0), &Calls](auto Result) mutable {
		EXPECT_EQ(Result.Status, EStatus::Ok); Calls = ++*State;
	});
	Owner.Wait(); EXPECT_EQ(Calls, 1u);
}



TEST(FBuildSessionTests, ReleasingSessionAndServiceDoesNotCancelOwnerRequests)
{
	auto Service = CreateBuild();
	auto Function = std::make_shared<FFunction>(); ASSERT_TRUE(Service->Register(Function));
	auto Producer = std::make_shared<FStageScheduler>();
	auto Session = std::make_shared<FBuildSession>(Service->CreateSession("DetachedSession", {}, Producer));
	FRequestOwner Owner; uint32 Calls = 0;
	Session->Build(Definition(), {}, Options(), Owner, [&](auto Completion) { EXPECT_EQ(Completion.Status, EStatus::Ok); ++Calls; });
	Session.reset(); Service.reset();
	EXPECT_FALSE(Owner.Poll()); EXPECT_EQ(Calls, 0u);
	Producer->Run(); Owner.Wait();
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll());
}

namespace
{
	class FOwnerTestRequest final : public IRequest, public Durin::DerivedData::Private::ICancelableRequest
	{
	public:
		std::function<void(ERequestPriority)> OnPriority;
		std::shared_ptr<bool> Destroyed;
		~FOwnerTestRequest() override { if (Destroyed) *Destroyed = true; }
		auto RequestCancel() -> void override {}
		auto Cancel() -> void override {}
		auto Wait() -> void override {}
		auto SetPriority(ERequestPriority Priority) -> void override { if (OnPriority) OnPriority(Priority); }
	};
}

TEST(FRequestOwnerTests, EndCallbackRetainsRequestAndGuardsContinuationSubmission)
{
	FRequestOwner Owner;
	auto Request = MakeRefCount<FOwnerTestRequest>();
	auto Destroyed = std::make_shared<bool>(false); Request->Destroyed = Destroyed;
	static_cast<IRequestOwner&>(Owner).Begin(Request);
	auto* Raw = Request.GetReference(); Request = nullptr;
	auto Next = MakeRefCount<FOwnerTestRequest>();
	auto Retained = static_cast<IRequestOwner&>(Owner).End(Raw, [&, Capture = std::make_unique<int>(42)] {
		EXPECT_EQ(*Capture, 42);
		EXPECT_FALSE(*Destroyed);
		EXPECT_FALSE(Owner.Poll());
		static_cast<IRequestOwner&>(Owner).Begin(Next);
	});
	EXPECT_FALSE(Owner.Poll());
	Retained = nullptr; EXPECT_TRUE(*Destroyed);
	static_cast<IRequestOwner&>(Owner).End(Next.GetReference()); Owner.Wait(); EXPECT_TRUE(Owner.Poll());
}

TEST(FRequestOwnerTests, EndCallbackExceptionReleasesBarrier)
{
	FRequestOwner Owner;
	auto Request = MakeRefCount<FOwnerTestRequest>(); static_cast<IRequestOwner&>(Owner).Begin(Request);
	EXPECT_THROW(static_cast<IRequestOwner&>(Owner).End(Request.GetReference(), [] { throw std::runtime_error("callback"); }), std::runtime_error);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FRequestOwnerTests, ConcurrentPriorityUpdatesConvergeWithoutHoldingOwnerLocks)
{
	FRequestOwner Owner;
	auto Request = MakeRefCount<FOwnerTestRequest>(); static_cast<IRequestOwner&>(Owner).Begin(Request);
	FThreadEvent LowEntered, ReleaseLow, HighStarted, HighApplied;
	std::atomic<ERequestPriority> Applied = ERequestPriority::Normal;
	Request->OnPriority = [&](ERequestPriority Priority) {
		if (Priority == ERequestPriority::Low) { LowEntered.Trigger(); ReleaseLow.Wait(); }
		Applied = Priority;
		if (Priority == ERequestPriority::High) HighApplied.Trigger();
	};
	std::jthread Low([&] { Owner.SetPriority(ERequestPriority::Low); });
	LowEntered.Wait();
	std::jthread High([&] { HighStarted.Trigger(); Owner.SetPriority(ERequestPriority::High); });
	HighStarted.Wait();
	EXPECT_TRUE(HighApplied.WaitFor(1.0));
	ReleaseLow.Trigger(); Low.join(); High.join();
	EXPECT_EQ(Owner.GetPriority(), ERequestPriority::High);
	EXPECT_EQ(Applied.load(), ERequestPriority::High);
	static_cast<IRequestOwner&>(Owner).End(Request.GetReference());
}

TEST(FBuildSessionTests, CanceledOwnerAlsoControlsValidationFailure)
{
	FFixture Fixture; auto Session = Fixture.Session();
	FRequestOwner Owner; Owner.Cancel(); uint32 Calls = 0;
	Session->Build(Definition(), {}, FBuildPolicy(EBuildPolicy(0x80)), Owner, [&](auto Result) {
		++Calls; EXPECT_EQ(Result.Status, EStatus::Canceled); EXPECT_FALSE(Owner.Poll());
	});
	Owner.Wait(); EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll());
}

TEST(FRequestOwnerTests, SingleWorkerCancellationHelpsRetiringBuildDispatch)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture; auto Session = Fixture.Session();
	FThreadEvent Finished; std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("CancelQueuedBuild", [&] {
		FRequestOwner Owner;
		Session->Build(Definition(), {}, Options(), Owner, [&](auto Result) {
			EXPECT_EQ(Result.Status, EStatus::Canceled); ++Calls;
		});
		Owner.Cancel(); EXPECT_TRUE(Owner.Poll()); Finished.Trigger();
	});
	EXPECT_TRUE(Finished.WaitFor(5.0));
	Parent.Wait(); EXPECT_EQ(Calls, 1u);
}

TEST(FRequestOwnerTests, LateRegistrationReceivesPriorityChangedInsideBarrier)
{
	FRequestOwner Owner(ERequestPriority::Low);
	auto Request = MakeRefCount<FOwnerTestRequest>();
	auto Applied = Owner.GetPriority();
	Request->OnPriority = [&](ERequestPriority Priority) { Applied = Priority; };
	{
		FRequestBarrier Submission(Owner, ERequestBarrierFlags::Priority);
		Owner.SetPriority(ERequestPriority::High);
		static_cast<IRequestOwner&>(Owner).Begin(Request);
		EXPECT_EQ(Applied, ERequestPriority::High);
		EXPECT_FALSE(Owner.Poll());
		static_cast<IRequestOwner&>(Owner).End(Request.GetReference());
	}
	EXPECT_TRUE(Owner.Poll());
}

TEST(FRequestOwnerTests, MoveTransfersControlAndSupportsRequestView)
{
	FRequestOwner First;
	IRequestOwner& Interface = First;
	auto Request = MakeRefCount<FOwnerTestRequest>(); Interface.Begin(Request.GetReference());
	FRequestOwner Moved(std::move(First));
	EXPECT_FALSE(Moved.Poll());
	TRefCountPtr<IRequest> Control(static_cast<IRequest*>(Moved));
	Control->SetPriority(ERequestPriority::High);
	EXPECT_EQ(Moved.GetPriority(), ERequestPriority::High);
	Interface.End(Request.GetReference());
	Moved.Wait(); EXPECT_TRUE(Moved.Poll());
	FRequestOwner Assigned; Assigned = std::move(Moved); EXPECT_TRUE(Assigned.Poll());
}

TEST(FRequestOwnerTests, KeepAliveAllowsQueuedTaskToOutliveFacade)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FThreadEvent Entered, Release, FacadeDestroyed, Finished;
	auto Blocker = Tasks::LaunchTask("KeepAliveBlocker", [&] { Entered.Trigger(); Release.Wait(); });
	Entered.Wait();
	std::atomic<uint32> Calls = 0;
	std::jthread Submitter([&] {
		{ FRequestOwner Owner; Owner.LaunchTask("KeepAliveBody", [&] { ++Calls; Finished.Trigger(); }); Owner.KeepAlive(); }
		FacadeDestroyed.Trigger();
	});
	EXPECT_TRUE(FacadeDestroyed.WaitFor(5.0));
	EXPECT_EQ(Calls, 0u); Release.Trigger(); Submitter.join();
	EXPECT_TRUE(Finished.WaitFor(5.0)); Blocker.Wait(); EXPECT_EQ(Calls, 1u);
}

TEST(FBuildSessionTests, ServiceBuildLimitsApplyWithoutRequestOptions)
{
	FBuildServiceOptions Options; Options.BuildLimits.Output.MaximumTotalBytes = 1;
	auto Service = CreateBuild(std::move(Options)); ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
	auto Session = Service->CreateSession("BoundedService"); FRequestOwner Owner; uint32 Calls = 0;
	Session.Build(Definition(), {}, EBuildPolicy::Build, Owner, [&](auto Result) { ++Calls; EXPECT_EQ(Result.Status, EStatus::Error); });
	Owner.Wait(); EXPECT_EQ(Calls, 1u);
}

TEST(FRequestOwnerTests, KeepAlivePreservesPublicOwnerUntilExternalRequestEnds)
{
	IRequestOwner* Shared = nullptr;
	auto Request = MakeRefCount<FOwnerTestRequest>();
	{
		FRequestOwner Owner(ERequestPriority::High);
		Shared = &static_cast<IRequestOwner&>(Owner);
		Shared->Begin(Request);
		Owner.KeepAlive();
	}
	EXPECT_EQ(Shared->GetPriority(), ERequestPriority::High);
	EXPECT_FALSE(Shared->IsCanceled());
	auto Retained = Shared->End(Request);
	EXPECT_EQ(Retained.GetReference(), Request.GetReference());
}

TEST(FRequestOwnerTests, KeepAlivePreservesPublicOwnerUntilBarrierEnds)
{
	IRequestOwner* Shared = nullptr;
	std::unique_ptr<FRequestBarrier> Barrier;
	{
		FRequestOwner Owner(ERequestPriority::Low);
		Shared = &static_cast<IRequestOwner&>(Owner);
		Barrier = std::make_unique<FRequestBarrier>(*Shared);
		Owner.KeepAlive();
	}
	EXPECT_EQ(Shared->GetPriority(), ERequestPriority::Low);
	Barrier.reset();
}



TEST(FBuildSessionTests, PublicOwnerProtocolSupportsIndependentImplementations)
{
	class FOwner final : public IRequestOwner
	{
	public:
		FRequestOwner Control;
		auto Begin(IRequest* Request) -> void override { static_cast<IRequestOwner&>(Control).Begin(Request); }
		auto End(IRequest* Request) -> TRefCountPtr<IRequest> override { return static_cast<IRequestOwner&>(Control).End(Request); }
		auto BeginBarrier(const FRequestBarrier& Barrier) -> void override { static_cast<IRequestOwner&>(Control).BeginBarrier(Barrier); }
		auto EndBarrier(const FRequestBarrier& Barrier) -> void override { static_cast<IRequestOwner&>(Control).EndBarrier(Barrier); }
		auto GetPriority() const -> ERequestPriority override { return Control.GetPriority(); }
		auto IsCanceled() const -> bool override { return Control.IsCanceled(); }
	};
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	class FResolver final : public IBuildInputResolver
	{
	public:
		auto ResolveInputMeta(const FBuildDefinition&, IRequestOwner& Owner, FOnBuildInputMetaResolved&& Notify) const -> void override
		{ Owner.LaunchTask("ResolveMeta", [Notify = std::move(Notify)]() mutable { Notify({{{"Source", FXxHash128::HashBuffer("input"), "Fixture", 1, "Bytes", 1}}, EStatus::Ok}); }); }
		auto ResolveInputData(const FBuildAction& Action, IRequestOwner& Owner, FOnBuildInputDataResolved&& Notify) const -> void override
		{ auto Identity = Action.GetInputs().front(); Owner.LaunchTask("ResolveData", [Identity, Notify = std::move(Notify)]() mutable { Notify({{{.Identity = Identity, .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(8))}}}}, EStatus::Ok}); }); }
	};
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession("Protocol", std::make_shared<FResolver>());
	FBuildDefinitionBuilder Builder("Session.Fixture"); Builder.AddInput("Source", "Capture"); auto Request = std::move(Builder).Build().value();
	FOwner Owner; std::atomic_uint Calls = 0;
	Session.Build(Request, {}, {}, Owner, [&](auto Result) { EXPECT_EQ(Result.Status, EStatus::Ok); ++Calls; });
	Owner.Control.Wait(); EXPECT_EQ(Calls, 1u);
	{ FRequestBarrier Submission(Owner); Owner.LaunchTask("ProtocolTask", [&] { ++Calls; }); } Owner.Control.Wait(); EXPECT_EQ(Calls, 2u);
}

TEST(FRequestOwnerTests, PriorityPropagationAllowsReentryAndRegistersContinuations)
{
	FRequestOwner Owner;
	auto Request = MakeRefCount<FOwnerTestRequest>(), Next = MakeRefCount<FOwnerTestRequest>();
	static_cast<IRequestOwner&>(Owner).Begin(Request);
	bool Registered = false; auto Applied = ERequestPriority::Normal;
	Next->OnPriority = [&](ERequestPriority Value) { Applied = Value; };
	Request->OnPriority = [&](ERequestPriority Value) {
		Owner.SetPriority(Value);
		if (!Registered) { Registered = true; static_cast<IRequestOwner&>(Owner).Begin(Next); }
	};
	{
		FRequestBarrier Barrier(Owner, ERequestBarrierFlags::Priority);
		Owner.SetPriority(ERequestPriority::High);
		EXPECT_TRUE(Registered); EXPECT_EQ(Applied, ERequestPriority::High);
		static_cast<IRequestOwner&>(Owner).End(Request); static_cast<IRequestOwner&>(Owner).End(Next);
	}
	EXPECT_TRUE(Owner.Poll());
}


TEST(FRequestOwnerTests, CancelJoinsScheduledTaskExecutionAndItsCallback)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FThreadEvent Entered, ReleaseWorker, BodyEntered, ReleaseBody, Returned;
	auto Blocker = Tasks::LaunchTask("BlockWorker", [&] { Entered.Trigger(); ReleaseWorker.Wait(); }); Entered.Wait();
	FRequestOwner Owner; std::atomic_uint Calls = 0;
	Owner.LaunchTask("QueuedOwnerTask", [&] {
		EXPECT_NE(Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId(), 0u);
		EXPECT_TRUE(Owner.IsCanceled()); BodyEntered.Trigger(); ReleaseBody.Wait(); ++Calls;
	});
	std::jthread Canceler([&] { Owner.Cancel(); Returned.Trigger(); });
	const bool Started = BodyEntered.WaitFor(5.0);
	if (!Started) ReleaseWorker.Trigger();
	EXPECT_TRUE(Started); EXPECT_FALSE(Returned.WaitFor(0.05));
	ReleaseBody.Trigger(); ReleaseWorker.Trigger(); Canceler.join(); Blocker.Wait();
	EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildSessionTests, ResolverFailureAndCancellationUseTheCompletionProtocol)
{
	class FResolver final : public IBuildInputResolver
	{
	public:
		EStatus Meta = EStatus::Ok, Data = EStatus::Error;
		auto ResolveInputMeta(const FBuildDefinition&, IRequestOwner&, FOnBuildInputMetaResolved&& Notify) const -> void override
		{ Notify({{{"Source", FXxHash128::HashBuffer("input"), "Fixture", 1, "Bytes", 1}}, Meta}); }
		auto ResolveInputData(const FBuildAction&, IRequestOwner&, FOnBuildInputDataResolved&& Notify) const -> void override { Notify({{}, Data}); }
	};
	for (bool Metadata : {false, true}) for (const auto Status : {EStatus::Error, EStatus::Canceled}) {
		FFixture Fixture; auto Resolver = std::make_shared<FResolver>();
		if (Metadata) Resolver->Meta = Status; else Resolver->Data = Status;
		auto Session = Fixture.Service->CreateSession("ResolveStatus", Resolver);
		FBuildDefinitionBuilder Builder("Session.Fixture"); Builder.AddInput("Source", "Capture"); auto Request = std::move(Builder).Build().value();
		FRequestOwner Owner; uint32 Calls = 0;
		Session.Build(Request, {}, EBuildPolicy::Build, Owner, [&](auto Result) {
			++Calls; EXPECT_EQ(Result.Status, Status); EXPECT_TRUE(Result.Output.IsValid()); EXPECT_TRUE(Result.Output.HasError());
			EXPECT_EQ(Result.CacheKey.IsValid(), !Metadata); EXPECT_FALSE(Owner.IsCanceled());
		});
		Owner.Wait(); EXPECT_EQ(Calls, 1u); EXPECT_EQ(Fixture.Function->Builds, 0u);
	}
}

TEST(FRequestOwnerTests, NonblockingCancellationDoesNotExecuteQueuedNativeTasks)
{
	ShutdownTaskScheduler(false); ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FRequestOwner Owner; uint32 Calls = 0; std::atomic_bool Signaling = false;
	FThreadEvent Entered, Release, Signaled;
	Owner.LaunchTask("SignalCancellation", [&] {
		Entered.Trigger(); Release.Wait(); Signaling = true;
		Durin::DerivedData::Private::FOwnerAccess::RequestCancel(Owner);
		Signaling = false; Signaled.Trigger();
	});
	Entered.Wait();
	Owner.LaunchTask("QueuedNativeTask", [&] { EXPECT_FALSE(Signaling.load()); EXPECT_TRUE(Owner.IsCanceled()); ++Calls; });
	Release.Trigger(); Signaled.Wait();
	Owner.Wait(); EXPECT_EQ(Calls, 1u); EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildSessionTests, DeferredSubmissionExceptionsCompleteWithError)
{
	class FResolver final : public IBuildInputResolver
	{
	public:
		auto ResolveInputMeta(const FBuildDefinition&, IRequestOwner&, FOnBuildInputMetaResolved&&) const -> void override { throw std::runtime_error("metadata failure"); }
		auto ResolveInputData(const FBuildAction&, IRequestOwner&, FOnBuildInputDataResolved&&) const -> void override { FAIL() << "Unexpected payload resolution"; }
	};
	class FCache final : public ICache
	{
	public:
		auto Get(std::span<const FCacheGetRequest>, IRequestOwner&, FOnCacheGetComplete&&) -> void override { throw std::runtime_error("query failure"); }
		auto Put(std::span<const FCachePutRequest>, IRequestOwner&, FOnCachePutComplete&&) -> void override { FAIL() << "Unexpected store"; }
	};
	for (bool Resolve : {false, true}) {
		auto Scheduler = std::make_shared<FStageScheduler>();
		auto Service = CreateBuild({.Cache = std::make_shared<FCache>()}); ASSERT_TRUE(Service->Register(std::make_shared<FFunction>()));
		auto Session = Service->CreateSession("ThrowingStage", std::make_shared<FResolver>(), Scheduler);
		FBuildDefinitionBuilder Builder("Session.Fixture"); if (Resolve) Builder.AddInput("Source", "Capture");
		FRequestOwner Owner; uint32 Calls = 0;
		Session.Build(std::move(Builder).Build().value(), {}, {}, Owner, [&](auto Result) {
			++Calls; EXPECT_EQ(Result.Status, EStatus::Error); EXPECT_TRUE(Result.Output.HasError());
		});
		EXPECT_EQ(Calls, 0u); Scheduler->Run(); Owner.Wait(); EXPECT_EQ(Calls, 1u);
	}
}

TEST(FBuildSessionTests, EarlyCompletionReplaysPriorityToContinuations)
{
	FFixture Fixture; auto Session = Fixture.Session(); FRequestOwner Owner(ERequestPriority::Low);
	auto Next = MakeRefCount<FOwnerTestRequest>(); ERequestPriority Applied = ERequestPriority::Low;
	Next->OnPriority = [&](auto Priority) { Applied = Priority; };
	Session->Build(Definition(), {}, EBuildPolicy(uint8{0x80}), Owner, [&](auto Result) {
		EXPECT_EQ(Result.Status, EStatus::Error); Owner.SetPriority(ERequestPriority::High);
		static_cast<IRequestOwner&>(Owner).Begin(Next);
	});
	EXPECT_EQ(Applied, ERequestPriority::High);
	static_cast<IRequestOwner&>(Owner).End(Next); Owner.Wait();
}

#include "DerivedDataBuildSession.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	struct FFunction final : IBuildFunction
	{
		mutable std::atomic<uint32> Builds = 0;
		std::function<void()> OnBuild;
		auto GetName() const -> std::string_view override { return "Session.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& Context) const -> void override
		{ Context.SetConstantsSchema(1); Context.SetOutput("Fixture.Output", 1); Context.SetCacheBucket(FCacheBucket::FromString("Sessions")); }
		auto Build(FBuildContext& Context) const -> void override
		{
			++Builds; if (OnBuild) OnBuild();
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
	FBuildRequestOwner Owner;
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
		EXPECT_EQ(Request->Wait(), EBuildWaitResult::Completed);
	});
	ASSERT_TRUE(DispatchStarted.WaitFor(2.0));
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(CallbackStarted.WaitFor(2.0));
	std::jthread Waiter([&] { EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed); WaitReturned.Trigger(); });
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
	FBuildRequestOwner Owner;
	uint32 Calls = 0;
	auto Completed = [&](FBuildCompleteParams Result) { ++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); };
	auto Request = First->Build(Definition(), Owner, Completed, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_FALSE(Owner.Poll());
	Owner.Cancel();
	ASSERT_TRUE(Second->Build(Definition(), Owner, Completed, {}, Options()));
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
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
		FBuildRequestOwner Owner;
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
	FBuildRequestOwner Owner;
	FBuildRequest Handle;
	auto Request = Session->Build(Definition(), Owner, [&](auto) {
		EXPECT_EQ(Handle.Wait(), EBuildWaitResult::WouldBlock);
		EXPECT_EQ(Owner.Wait(), EBuildWaitResult::WouldBlock);
	}, {}, Options());
	ASSERT_TRUE(Request); Handle = *Request;
	Queued();
	EXPECT_EQ(Handle.Wait(), EBuildWaitResult::Completed);
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
}

TEST(FBuildSessionTests, BarrierProtectsSubmissionAfterOwnerTemporarilyBecomesIdle)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	FBuildRequestOwner Owner;
	FThreadEvent BarrierOpened, AllowSubmission, WaitReturned;
	std::jthread Submitter([&] {
		FBuildRequestBarrier Barrier(Owner);
		EXPECT_EQ(Owner.Wait(), EBuildWaitResult::WouldBlock);
		BarrierOpened.Trigger();
		EXPECT_TRUE(AllowSubmission.WaitFor(2.0));
		ASSERT_TRUE(Session->Build(Definition(), Owner, [](auto) {}, {}, Options()));
	});
	ASSERT_TRUE(BarrierOpened.WaitFor(2.0));
	EXPECT_FALSE(Owner.Poll());
	std::jthread Waiter([&] { EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed); WaitReturned.Trigger(); });
	EXPECT_FALSE(WaitReturned.WaitFor(0.05));
	AllowSubmission.Trigger(); Submitter.join(); Waiter.join();
	EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
	EXPECT_TRUE(Owner.Poll());
}

TEST(FBuildSessionTests, TaskSchedulerWaitHelpsNestedBuildWithOneWorker)
{
	ShutdownTaskScheduler(false);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler()).value();
	std::atomic<uint32> Calls = 0;
	auto Parent = Tasks::LaunchTask("NestedDerivedData", [&] {
		FBuildRequestOwner Owner(EBuildPriority::High);
		auto Request = Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
			EXPECT_EQ(Result.GetStatus(), EStatus::Ok); ++Calls;
		}, {}, Options());
		ASSERT_TRUE(Request);
		EXPECT_EQ(Owner.GetPriority(), EBuildPriority::High);
		EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
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
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler()).value();
	FBuildRequestOwner Owner;
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), Owner, [&](auto) { ++Calls; }, {}, Options());
	ASSERT_FALSE(Request);
	EXPECT_EQ(Request.error().Reason, EBuildAdmissionReason::DispatchRejected);
	EXPECT_EQ(Calls, 0u);
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
}

TEST(FBuildSessionTests, SchedulerReceivesOwnerPriorityAndExistingWorkingSetBudget)
{
	class FRecordingScheduler final : public IBuildScheduler
	{
	public:
		auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
		{
			EXPECT_EQ(Params.FunctionName, "Session.Fixture");
			EXPECT_EQ(Params.Priority, EBuildPriority::Low);
			EXPECT_EQ(Params.MaximumWorkingSetBytes, 12345u);
			Work();
			return std::shared_ptr<IBuildScheduledWork>{};
		}
	};
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, std::make_shared<FRecordingScheduler>()).value();
	FBuildRequestOwner Owner(EBuildPriority::Low);
	auto Policy = Options(); Policy.Policy.MaximumWorkingSetBytes = 12345;
	ASSERT_TRUE(Session->Build(Definition(), Owner, [](auto) {}, {}, std::move(Policy)));
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
}

TEST(FBuildSessionTests, BarrierMayBeReleasedOnAnotherThread)
{
	FBuildRequestOwner Owner;
	auto Barrier = std::make_unique<FBuildRequestBarrier>(Owner);
	EXPECT_FALSE(Owner.Poll());
	std::jthread Releaser([Barrier = std::move(Barrier)]() mutable { Barrier.reset(); });
	Releaser.join();
	EXPECT_TRUE(Owner.Poll());
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
}

TEST(FBuildSessionTests, CanceledOwnerDoesNotScheduleNewWork)
{
	ShutdownTaskScheduler(false);
	FFixture Fixture;
	auto Session = Fixture.Service->CreateSession({}, CreateTaskBuildScheduler()).value();
	FBuildRequestOwner Owner; Owner.Cancel();
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), Owner, [&](FBuildCompleteParams Result) {
		++Calls; EXPECT_EQ(Result.GetStatus(), EStatus::Canceled);
	}, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_EQ(Owner.Wait(), EBuildWaitResult::Completed);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
}

TEST(FBuildSessionTests, InvalidRequestWaitDoesNotReportCompletion)
{
	FBuildRequest Request;
	EXPECT_FALSE(Request.IsComplete());
	EXPECT_EQ(Request.Wait(), EBuildWaitResult::InvalidRequest);
}

TEST(FBuildSessionTests, OwnerMayBeDestroyedInsideItsCompletion)
{
	FFixture Fixture;
	auto Session = Fixture.Session();
	auto Owner = std::make_unique<FBuildRequestOwner>();
	uint32 Calls = 0;
	auto Request = Session->Build(Definition(), *Owner, [&](auto) {
		++Calls; Owner.reset();
	}, {}, Options());
	ASSERT_TRUE(Request);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Request->Wait(), EBuildWaitResult::Completed);
	EXPECT_EQ(Session->Drain(), EBuildDrainResult::Drained);
}

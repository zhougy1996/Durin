#include "DerivedDataBuildSession.h"
#include "Threading/ThreadEvent.h"
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
	auto Definition(std::string Name = "Session.Fixture") -> FBuildDefinition
	{ return std::move(FBuildDefinitionBuilder(std::move(Name))).Build().value(); }
	auto Options() -> FBuildRequestOptions
	{ return {.Policy = {.QueryCache = false, .StoreOnBuild = false}}; }
	struct FFixture
	{
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		std::shared_ptr<IBuild> Service = CreateBuild();
		FFixture() { Service->Register(Function).value(); }
		auto Session(FBuildDispatcher Dispatcher = {}) -> std::shared_ptr<FBuildSession>
		{ return Service->CreateSession({}, std::move(Dispatcher)).value(); }
	};
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

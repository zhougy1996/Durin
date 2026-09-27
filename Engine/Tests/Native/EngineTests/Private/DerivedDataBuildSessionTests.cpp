#include "DerivedDataBuildSession.h"
#include "Threading/TaskComposition.h"
#include "Threading/ThreadEvent.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	struct FResolver final : IBuildInputResolver
	{
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildError> override { return std::vector<FBuildInputReference>{}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInput>, FBuildError> override { return std::vector<FBuildInput>{}; }
	};
	struct FFunction final : IBuildFunction
	{
		mutable std::atomic<uint32> Builds = 0;
		std::function<void()> OnBuild;
		auto GetDescriptor() const -> FBuildFunctionDescriptor override
		{ return {"Session.Fixture", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("Sessions")}; }
		auto Build(FBuildContext&) const -> std::expected<FBuildOutput, FBuildError> override
		{
			++Builds; if (OnBuild) OnBuild();
			return FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1,
				.Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(32, std::byte{7}))}}}).value();
		}
		auto Validate(const FBuildAction&, const FBuildOutput&, const FBuildCancellation&) const -> std::expected<void, FBuildError> override { return {}; }
	};
	auto Definition() -> FBuildDefinition { return FBuildDefinition::TryCreate("Session.Fixture", {}, {}).value(); }
	auto Options() -> FBuildRequestOptions { return {.Policy = {.ReadCache = false, .WriteCache = false}}; }
	struct FFixture
	{
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		std::shared_ptr<FResolver> Resolver = std::make_shared<FResolver>();
		auto Registry() -> FBuildRegistrySnapshot
		{ FBuildRegistry Registry; Registry.Register(Function).value(); return Registry.Freeze().value(); }
	};
}

TEST(FBuildSessionTests, InlineCompletionReentersWithoutLocksAndSelfDrainIsExplicit)
{
	FFixture Fixture;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver);
	uint32 Calls = 0;
	auto Request = Session.Submit(Definition(), [&](FBuildResult Result) {
		++Calls; EXPECT_TRUE(Result);
		auto Nested = Session.ExecuteInline(Definition(), Options());
		ASSERT_TRUE(Nested);
		EXPECT_EQ(Session.Drain(), EBuildDrainResult::WouldBlock);
	}, Options());
	ASSERT_TRUE(Request); EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
	EXPECT_FALSE(Request->Cancel());
	EXPECT_FALSE(Session.Submit(Definition(), [&](auto) { ++Calls; }, Options()));
	EXPECT_EQ(Calls, 1u); EXPECT_EQ(Fixture.Function->Builds.load(), 2u);
}

TEST(FBuildSessionTests, RejectedDispatchNeverCallsCompletionAndDroppedAcceptedWorkCancels)
{
	for (uint32 Mode = 0; Mode < 3; ++Mode)
	{
		FFixture Fixture;
		FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [=](auto) -> std::expected<void, FBuildError> {
			if (Mode == 0) return std::unexpected(FBuildError{.Description = "Rejected"});
			if (Mode == 1) throw std::bad_alloc();
			return {};
		});
		uint32 Calls = 0;
		auto Request = Session.Submit(Definition(), [&](auto Result) { ++Calls; EXPECT_TRUE(IsBuildCancelled(Result)); }, Options());
		EXPECT_EQ(Request.has_value(), Mode == 2); EXPECT_EQ(Calls, Mode == 2 ? 1u : 0u);
		EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
		EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
	}
}

TEST(FBuildSessionTests, CancelledQueuedHandlesAndStaleThunksDoNotRetainProviders)
{
	FFixture Fixture;
	std::weak_ptr<FFunction> Function = Fixture.Function;
	std::weak_ptr<FResolver> Resolver = Fixture.Resolver;
	std::function<void()> Queued;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> { Queued = std::move(Work); return {}; });
	Fixture.Function.reset(); Fixture.Resolver.reset();
	uint32 Calls = 0;
	auto Request = Session.Submit(Definition(), [&](auto Result) { ++Calls; EXPECT_TRUE(IsBuildCancelled(Result)); }, Options());
	ASSERT_TRUE(Request); EXPECT_FALSE(Request->IsComplete()); EXPECT_TRUE(Request->Cancel());
	EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
	EXPECT_TRUE(Function.expired()); EXPECT_TRUE(Resolver.expired());
	Queued(); Queued(); Queued = {};
	EXPECT_EQ(Calls, 1u); EXPECT_FALSE(Request->Cancel());
}

TEST(FBuildSessionTests, CancellationAndRunnerRaceHasExactlyOneTerminalCallback)
{
	FFixture Fixture;
	std::function<void()> Queued;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> { Queued = std::move(Work); return {}; });
	for (uint32 Round = 0; Round < 100; ++Round)
	{
		std::atomic<uint32> Calls = 0;
		std::atomic<bool> WasCancelled = false;
		auto Request = Session.Submit(Definition(), [&](auto Result) { WasCancelled = IsBuildCancelled(Result); ++Calls; }, Options());
		ASSERT_TRUE(Request);
		std::barrier Start(2);
		std::jthread Worker([&] { Start.arrive_and_wait(); Queued(); });
		Start.arrive_and_wait(); const bool AcceptedCancel = Request->Cancel(); Worker.join();
		EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls.load(), 1u);
		EXPECT_EQ(WasCancelled.load(), AcceptedCancel);
		Queued = {};
	}
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
}

TEST(FBuildSessionTests, RunningCloseDrainsCallbacksBeforeReleasingServices)
{
	FFixture Fixture;
	FThreadEvent Started, Release, CallbackFinished;
	Fixture.Function->OnBuild = [&] { Started.Trigger(); EXPECT_TRUE(Release.WaitFor(2.0)); };
	std::function<void()> Queued;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> { Queued = std::move(Work); return {}; });
	auto Request = Session.Submit(Definition(), [&](auto Result) {
		EXPECT_TRUE(IsBuildCancelled(Result)); CallbackFinished.Trigger();
	}, Options());
	ASSERT_TRUE(Request);
	std::jthread Worker([&] { Queued(); });
	ASSERT_TRUE(Started.WaitFor(2.0)); Session.Close();
	EXPECT_FALSE(Request->IsComplete()); Release.Trigger();
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
	EXPECT_TRUE(CallbackFinished.WaitFor(0.0)); EXPECT_TRUE(Request->IsComplete());
	Worker.join();
}

TEST(FBuildSessionTests, OneCoreWorkerExecutesNestedInlineBuildWithoutQueueing)
{
	ShutdownTaskScheduler(true);
	ASSERT_TRUE(InitializeTaskScheduler(1));
	struct FShutdown { ~FShutdown() { ShutdownTaskScheduler(true); } } Shutdown;
	FFixture Fixture;
	uint32 Dispatches = 0;
	FThreadEvent Finished;
	std::vector<Tasks::TTask<void>> Tasks;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> {
		++Dispatches; Tasks.push_back(Durin::Tasks::LaunchTask("BuildSessionFixture", std::move(Work))); return {};
	});
	auto Request = Session.Submit(Definition(), [&](auto Result) {
		EXPECT_TRUE(Result);
		auto Nested = Session.ExecuteInline(Definition(), Options());
		EXPECT_TRUE(Nested);
		Finished.Trigger();
	}, Options());
	ASSERT_TRUE(Request); ASSERT_TRUE(Finished.WaitFor(2.0));
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
	EXPECT_EQ(Dispatches, 1u); EXPECT_EQ(Fixture.Function->Builds.load(), 2u);
	for (auto& Task : Tasks) EXPECT_EQ(Task.Wait().TaskState, ETaskState::Succeeded);
}

TEST(FBuildSessionTests, CallbackExceptionsAndSessionDestructionInsideCallbackStillDrain)
{
	FFixture Fixture;
	auto Session = std::make_unique<FBuildSession>(Fixture.Registry(), Fixture.Resolver);
	uint32 Calls = 0;
	auto Request = Session->Submit(Definition(), [&](auto) { ++Calls; Session.reset(); throw std::runtime_error("callback"); }, Options());
	ASSERT_TRUE(Request); EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls, 1u); EXPECT_FALSE(Session);
}

TEST(FBuildSessionTests, CloseWaitsForDispatchReturnBeforeReleasingTheRegistry)
{
	FFixture Fixture;
	std::weak_ptr<FFunction> Lifetime = Fixture.Function;
	FThreadEvent Invoked, Release;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> {
		Work(); Invoked.Trigger(); EXPECT_TRUE(Release.WaitFor(2.0)); return {};
	});
	Fixture.Function.reset(); Fixture.Resolver.reset();
	std::atomic<uint32> Calls = 0;
	std::jthread Submitter([&] {
		auto Request = Session.Submit(Definition(), [&](auto Result) { EXPECT_TRUE(Result); ++Calls; }, Options());
		EXPECT_TRUE(Request); if (Request) EXPECT_TRUE(Request->IsComplete());
	});
	ASSERT_TRUE(Invoked.WaitFor(2.0)); Session.Close();
	EXPECT_EQ(Calls.load(), 1u); EXPECT_FALSE(Lifetime.expired());
	Release.Trigger(); Submitter.join();
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained); EXPECT_TRUE(Lifetime.expired());
}

TEST(FBuildSessionTests, CloseRacingAdmissionEitherRejectsOrCancelsExactlyOnce)
{
	for (uint32 Round = 0; Round < 50; ++Round)
	{
		FFixture Fixture;
		std::function<void()> Queued;
		FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> { Queued = std::move(Work); return {}; });
		std::barrier Start(2);
		std::atomic<uint32> Calls = 0;
		bool Accepted = false;
		std::jthread Submitter([&] {
			Start.arrive_and_wait();
			auto Request = Session.Submit(Definition(), [&](auto Result) { EXPECT_TRUE(IsBuildCancelled(Result)); ++Calls; }, Options());
			Accepted = Request.has_value();
		});
		Start.arrive_and_wait(); Session.Close(); Submitter.join();
		EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
		EXPECT_EQ(Calls.load(), Accepted ? 1u : 0u); EXPECT_EQ(Fixture.Function->Builds.load(), 0u);
		if (Queued) Queued();
		EXPECT_EQ(Calls.load(), Accepted ? 1u : 0u);
	}
}

TEST(FBuildSessionTests, CloseCancelsAllQueuedRequestsAndOutputOutlivesDrainedSession)
{
	FFixture Fixture;
	std::vector<std::function<void()>> Queue;
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver, [&](auto Work) -> std::expected<void, FBuildError> { Queue.push_back(std::move(Work)); return {}; });
	auto Output = Session.ExecuteInline(Definition(), Options());
	ASSERT_TRUE(Output);
	uint32 Calls = 0;
	std::vector<FBuildRequest> Requests;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		auto Request = Session.Submit(Definition(), [&](auto Result) { ++Calls; EXPECT_TRUE(IsBuildCancelled(Result)); }, Options());
		ASSERT_TRUE(Request); Requests.push_back(*Request);
	}
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained); EXPECT_EQ(Calls, 16u);
	for (const auto& Request : Requests) EXPECT_TRUE(Request.IsComplete());
	for (auto& Work : Queue) Work();
	EXPECT_EQ(Calls, 16u); EXPECT_EQ(Fixture.Function->Builds.load(), 1u);
	EXPECT_EQ(Output->FindValue("Data")->Data[0], std::byte{7});
}

TEST(FBuildSessionTests, AdmissionRejectionHasNoCallbackAndCallableExceptionsStillComplete)
{
	FFixture Fixture;
	uint32 Calls = 0;
	FBuildSession MissingResolver(Fixture.Registry(), {});
	EXPECT_FALSE(MissingResolver.Submit(Definition(), [&](auto) { ++Calls; }, Options()));
	FBuildSession Session(Fixture.Registry(), Fixture.Resolver);
	EXPECT_FALSE(Session.Submit(Definition(), {}, Options()));
	auto Unknown = FBuildDefinition::TryCreate("Missing.Function", {}, {}).value();
	EXPECT_FALSE(Session.Submit(std::move(Unknown), [&](auto) { ++Calls; }, Options()));
	EXPECT_EQ(Calls, 0u);
	Fixture.Function->OnBuild = [] { throw std::runtime_error("recipe failure"); };
	auto Request = Session.Submit(Definition(), [&](auto Result) {
		++Calls; EXPECT_FALSE(Result); EXPECT_EQ(Result.error().Category, EBuildErrorCategory::ProducerFailure);
	}, Options());
	ASSERT_TRUE(Request); EXPECT_TRUE(Request->IsComplete()); EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Session.Drain(), EBuildDrainResult::Drained);
}

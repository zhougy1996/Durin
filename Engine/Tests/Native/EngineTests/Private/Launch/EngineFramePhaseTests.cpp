#include "Runtime/Launch/Private/EngineFramePhases.h"

#include <gtest/gtest.h>

TEST(FEngineFramePhaseTests, ForegroundFrameCapIsOptionalAndLifecycleThrottlesRemain)
{
	using namespace Durin;
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bStartupReady = false, .bEditor = true}), 1.0 / 30.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true}), 0.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .FrameRateLimit = 60}), 1.0 / 60.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .FrameRateLimit = 144}), 1.0 / 144.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .FrameRateLimit = -1}), 0.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bMinimized = true, .FrameRateLimit = 144}), 1.0 / 5.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .FrameRateLimit = 144}), 1.0 / 30.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bStartupReady = false, .bEditor = true, .FrameRateLimit = 144}), 1.0 / 30.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.FrameRateLimit = 120}), 1.0 / 120.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bMinimized = true}), 1.0 / 5.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false}), 1.0 / 30.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bStartupReady = false, .bEditor = true, .bFocused = false}), 1.0 / 30.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({}), 0.0);
}

TEST(FEngineFramePhaseTests, BackgroundCapCanBeConfiguredOrDisabledWithoutRaisingNormalLimit)
{
	using namespace Durin;
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .BackgroundFrameRateLimit = 15}), 1.0 / 15.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .FrameRateLimit = 20}), 1.0 / 20.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .FrameRateLimit = 60, .BackgroundFrameRateLimit = 0}), 1.0 / 60.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .BackgroundFrameRateLimit = 0}), 0.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .FrameRateLimit = 144, .BackgroundFrameRateLimit = 15}), 1.0 / 144.0);
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bFocused = false, .FrameRateLimit = 120, .BackgroundFrameRateLimit = 15}), 1.0 / 120.0);
}

TEST(FEngineFramePhaseTests, ProfilerConnectionSuspendsOnlyBackgroundReduction)
{
	using namespace Durin;
	for (const int32 BackgroundLimit : {0, 15, 30})
	{
		EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .FrameRateLimit = 144, .BackgroundFrameRateLimit = BackgroundLimit, .bProfilerConnected = true}), 1.0 / 144.0);
		EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .BackgroundFrameRateLimit = BackgroundLimit, .bProfilerConnected = true}), 0.0);
		EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bMinimized = true, .bFocused = false, .FrameRateLimit = 144, .BackgroundFrameRateLimit = BackgroundLimit, .bProfilerConnected = true}), 1.0 / 5.0);
		EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bStartupReady = false, .bEditor = true, .bFocused = false, .FrameRateLimit = 144, .BackgroundFrameRateLimit = BackgroundLimit, .bProfilerConnected = true}), 1.0 / 30.0);
		EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bMinimized = true, .bFocused = false, .FrameRateLimit = 144, .BackgroundFrameRateLimit = BackgroundLimit}), 1.0 / 5.0);
	}
	// Disconnecting resumes the configured background cap without changing the normal cap.
	EXPECT_DOUBLE_EQ(GetHostFrameInterval({.bEditor = true, .bFocused = false, .FrameRateLimit = 144}), 1.0 / 30.0);
}

TEST(FEngineFramePhaseTests, FocusAndProfilerChangesReleaseTheOldBackgroundDeadline)
{
	using namespace Durin;
	FHostFrameState State{.bEditor = true, .bFocused = false, .FrameRateLimit = 60, .BackgroundFrameRateLimit = 1};
	constexpr double LastFrameTime = 10.0;
	constexpr double CurrentTime = 10.1;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, LastFrameTime, CurrentTime), 0.05);
	State.bFocused = true;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, LastFrameTime, CurrentTime), 0.0);
	State.bFocused = false;
	State.bProfilerConnected = true;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, LastFrameTime, CurrentTime), 0.0);
	State.bProfilerConnected = false;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, LastFrameTime, CurrentTime), 0.05);
	State.BackgroundFrameRateLimit = 0;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, LastFrameTime, CurrentTime), 0.0);
}

TEST(FEngineFramePhaseTests, RestoringWindowsReleasesMinimizedPacingButRespectsNormalLimit)
{
	using namespace Durin;
	FHostFrameState State{.bEditor = true, .bMinimized = true, .FrameRateLimit = 60};
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, 10.0, 10.1), 0.05);
	State.bMinimized = false;
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, 10.0, 10.1), 0.0);
	// A policy change only shortens the deadline to the normal cap, never bypasses it.
	EXPECT_NEAR(GetHostFrameWaitTimeout(State, 10.0, 10.005), 1.0 / 60.0 - 0.005, 1e-12);
}

TEST(FEngineFramePhaseTests, InputWakeupsDoNotAdvanceFramesOrPostponeTheirDeadline)
{
	using namespace Durin;
	const FHostFrameState State{.bEditor = true, .bFocused = false, .BackgroundFrameRateLimit = 1};
	// Simulate repeated input and timeout wakeups throughout one background frame.
	for (const double Elapsed : {0.001, 0.01, 0.1, 0.5, 0.9})
		EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, 10.0, 10.0 + Elapsed), 0.05);
	EXPECT_NEAR(GetHostFrameWaitTimeout(State, 10.0, 10.99), 0.01, 1e-12);
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, 10.0, 11.0), 0.0);
	EXPECT_DOUBLE_EQ(GetHostFrameWaitTimeout(State, 10.0, 11.1), 0.0);
}

TEST(FEngineFramePhaseTests, PumpsInputBeforeGameAndTicksUIAfterGame)
{
	std::vector<std::string> Phases;
	Durin::EInteractiveFrameState State = Durin::EInteractiveFrameState::Idle;
	EXPECT_TRUE(Durin::RunInteractiveFramePhases(
		State,
		[&Phases]() { Phases.emplace_back("input"); },
		[&Phases]() {
			Phases.emplace_back("game");
			Phases.emplace_back("ui");
		},
		[]() { return false; }));
	EXPECT_EQ(Phases, (std::vector<std::string>{"input", "game", "ui"}));
	EXPECT_EQ(State, Durin::EInteractiveFrameState::Idle);
}

TEST(FEngineFramePhaseTests, StopsBeforeGameWhenPlatformInputRequestsExit)
{
	std::vector<std::string> Phases;
	bool bExitRequested = false;
	Durin::EInteractiveFrameState State = Durin::EInteractiveFrameState::Idle;
	EXPECT_FALSE(Durin::RunInteractiveFramePhases(
		State,
		[&]() {
			Phases.emplace_back("input");
			bExitRequested = true;
		},
		[&Phases]() { Phases.emplace_back("frame"); },
		[&bExitRequested]() { return bExitRequested; }));
	EXPECT_EQ(Phases, (std::vector<std::string>{"input"}));
	EXPECT_EQ(State, Durin::EInteractiveFrameState::Idle);
}

TEST(FEngineFramePhaseTests, RunsModalContinuationWithoutRepumpingPlatformEvents)
{
	std::vector<std::string> Phases;
	Durin::EInteractiveFrameState State = Durin::EInteractiveFrameState::PumpingPlatformEvents;
	EXPECT_TRUE(Durin::TryRunModalContinuationFrame(
		State,
		true,
		[&]() {
			EXPECT_EQ(State, Durin::EInteractiveFrameState::RunningModalFrame);
			Phases.emplace_back("game");
			Phases.emplace_back("ui");
			Phases.emplace_back("render");
			Phases.emplace_back("maintenance");
		}));
	EXPECT_EQ(Phases, (std::vector<std::string>{"game", "ui", "render", "maintenance"}));
	EXPECT_EQ(State, Durin::EInteractiveFrameState::PumpingPlatformEvents);
}

TEST(FEngineFramePhaseTests, RejectsNestedAndClosedModalContinuations)
{
	Durin::EInteractiveFrameState State = Durin::EInteractiveFrameState::PumpingPlatformEvents;
	int FrameCount = 0;
	EXPECT_TRUE(Durin::TryRunModalContinuationFrame(State, true, [&]() {
		++FrameCount;
		EXPECT_FALSE(Durin::TryRunModalContinuationFrame(State, true, [&]() { ++FrameCount; }));
	}));
	EXPECT_EQ(FrameCount, 1);
	EXPECT_FALSE(Durin::TryRunModalContinuationFrame(State, false, [&]() { ++FrameCount; }));
	State = Durin::EInteractiveFrameState::ShuttingDown;
	EXPECT_FALSE(Durin::TryRunModalContinuationFrame(State, true, [&]() { ++FrameCount; }));
	EXPECT_EQ(FrameCount, 1);
}

TEST(FEngineFramePhaseTests, OuterFrameCompletesAfterModalContinuation)
{
	std::vector<std::string> Phases;
	Durin::EInteractiveFrameState State = Durin::EInteractiveFrameState::Idle;
	EXPECT_TRUE(Durin::RunInteractiveFramePhases(
		State,
		[&]() {
			Phases.emplace_back("input-begin");
			EXPECT_TRUE(Durin::TryRunModalContinuationFrame(
				State,
				true,
				[&]() { Phases.emplace_back("modal-frame"); }));
			Phases.emplace_back("input-end");
		},
		[&]() { Phases.emplace_back("outer-frame"); },
		[]() { return false; }));
	EXPECT_EQ(Phases, (std::vector<std::string>{
		"input-begin", "modal-frame", "input-end", "outer-frame"}));
}

#include "Runtime/Launch/Private/EngineFramePhases.h"

#include <gtest/gtest.h>

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

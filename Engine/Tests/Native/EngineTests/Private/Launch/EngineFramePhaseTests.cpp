#include "Runtime/Launch/Private/EngineFramePhases.h"

#include <gtest/gtest.h>

TEST(FEngineFramePhaseTests, PumpsInputBeforeGameAndTicksUIAfterGame)
{
	std::vector<std::string> Phases;
	EXPECT_TRUE(Durin::RunInteractiveFramePhases(
		[&Phases]() { Phases.emplace_back("input"); },
		[&Phases]() { Phases.emplace_back("game"); },
		[&Phases]() { Phases.emplace_back("ui"); },
		[]() { return false; }));
	EXPECT_EQ(Phases, (std::vector<std::string>{"input", "game", "ui"}));
}

TEST(FEngineFramePhaseTests, StopsBeforeGameWhenPlatformInputRequestsExit)
{
	std::vector<std::string> Phases;
	bool bExitRequested = false;
	EXPECT_FALSE(Durin::RunInteractiveFramePhases(
		[&]() {
			Phases.emplace_back("input");
			bExitRequested = true;
		},
		[&Phases]() { Phases.emplace_back("game"); },
		[&Phases]() { Phases.emplace_back("ui"); },
		[&bExitRequested]() { return bExitRequested; }));
	EXPECT_EQ(Phases, (std::vector<std::string>{"input"}));
}

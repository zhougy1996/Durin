#include "gtest/gtest.h"

#include "Editor/MainFrame/Public/Interfaces/IMainFrameModule.h"
#include "Engine/Engine.h"
#include "Runtime/Launch/Private/EngineFrame.h"

namespace Durin
{
	TEST(FEditorBootstrapStateTests, AllowsOnlyDocumentedForwardTransitions)
	{
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::ConstructingShell,
			EEditorBootstrapState::WaitingForFirstPresent));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::WaitingForFirstPresent,
			EEditorBootstrapState::LoadingWorkspace));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::WaitingForFirstPresent,
			EEditorBootstrapState::Ready));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::LoadingWorkspace,
			EEditorBootstrapState::WorkspaceReady));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::WorkspaceReady,
			EEditorBootstrapState::LoadingDefaultDocument));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::LoadingDefaultDocument,
			EEditorBootstrapState::Ready));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::LoadingDefaultDocument,
			EEditorBootstrapState::Failed));
		EXPECT_TRUE(IsValidEditorBootstrapTransition(
			EEditorBootstrapState::LoadingWorkspace,
			EEditorBootstrapState::Failed));
	}

	TEST(FEditorBootstrapStateTests, RejectsSkippedBackwardAndTerminalTransitions)
	{
		constexpr std::array States{
			EEditorBootstrapState::ConstructingShell,
			EEditorBootstrapState::WaitingForFirstPresent,
			EEditorBootstrapState::LoadingWorkspace,
			EEditorBootstrapState::WorkspaceReady,
			EEditorBootstrapState::LoadingDefaultDocument,
			EEditorBootstrapState::Ready,
			EEditorBootstrapState::Failed,
		};
		for (const EEditorBootstrapState From : States)
		{
			for (const EEditorBootstrapState To : States)
			{
				const bool bExpected =
					(From == EEditorBootstrapState::ConstructingShell
						&& To == EEditorBootstrapState::WaitingForFirstPresent)
					|| (From == EEditorBootstrapState::WaitingForFirstPresent
						&& (To == EEditorBootstrapState::LoadingWorkspace
							|| To == EEditorBootstrapState::Ready))
					|| (From == EEditorBootstrapState::LoadingWorkspace
						&& (To == EEditorBootstrapState::WorkspaceReady
							|| To == EEditorBootstrapState::Failed))
					|| (From == EEditorBootstrapState::WorkspaceReady
						&& To == EEditorBootstrapState::LoadingDefaultDocument)
					|| (From == EEditorBootstrapState::LoadingDefaultDocument
						&& (To == EEditorBootstrapState::Ready
							|| To == EEditorBootstrapState::Failed));
				EXPECT_EQ(IsValidEditorBootstrapTransition(From, To), bExpected);
			}
		}
	}

	TEST(FEditorBootstrapStateTests, EngineInitializationResultsRemainDistinct)
	{
		const FEngineInitializationResult Success =
			FEngineInitializationResult::Success();
		const FEngineInitializationResult Cancelled =
			FEngineInitializationResult::Cancelled("closed");
		const FEngineInitializationResult Failed =
			FEngineInitializationResult::Failure("load failed");

		EXPECT_TRUE(Success);
		EXPECT_FALSE(Cancelled);
		EXPECT_FALSE(Failed);
		EXPECT_EQ(Cancelled.Status, EEngineInitializationStatus::Cancelled);
		EXPECT_EQ(Cancelled.Message, "closed");
		EXPECT_EQ(Failed.Status, EEngineInitializationStatus::Failed);
		EXPECT_EQ(Failed.Message, "load failed");
	}

	TEST(FEditorBootstrapStateTests, MapsEveryStateToTruthfulPhaseAndStatus)
	{
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::ConstructingShell), 1);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::WaitingForFirstPresent), 1);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::LoadingWorkspace), 2);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::WorkspaceReady), 2);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::LoadingDefaultDocument), 3);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::Ready), 3);
		EXPECT_EQ(GetEditorBootstrapPhaseIndex(
			EEditorBootstrapState::Failed), 0);
		EXPECT_EQ(GetEditorBootstrapStepStatus(EEditorBootstrapState::Ready),
			EEditorBootstrapStepStatus::Ready);
		EXPECT_EQ(GetEditorBootstrapStepStatus(EEditorBootstrapState::Failed),
			EEditorBootstrapStepStatus::Failed);
		EXPECT_EQ(GetEditorBootstrapStepStatus(
			EEditorBootstrapState::LoadingDefaultDocument),
			EEditorBootstrapStepStatus::Pending);
	}

	TEST(FEditorBootstrapStateTests, StartupFramesExcludeEngineViewportRendering)
	{
		EXPECT_FALSE(ShouldRedrawEngineViewports(EEngineFrameMode::Startup));
		EXPECT_TRUE(ShouldRedrawEngineViewports(EEngineFrameMode::Running));
	}
}

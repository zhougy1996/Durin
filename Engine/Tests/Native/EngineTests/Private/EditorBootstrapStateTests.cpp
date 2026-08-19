#include "gtest/gtest.h"

#include "Editor/EditorHost.h"
#include "Engine/Engine.h"
#include "Runtime/Launch/Private/EngineFrame.h"

namespace Durin::Editor::Host
{
	TEST(FEditorBootstrapStateTests, AllowsOnlyDocumentedForwardTransitions)
	{
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::ConstructingShell,
			EBootstrapState::WaitingForFirstPresent));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::WaitingForFirstPresent,
			EBootstrapState::LoadingWorkspace));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::WaitingForFirstPresent,
			EBootstrapState::Ready));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::LoadingWorkspace,
			EBootstrapState::WorkspaceReady));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::WorkspaceReady,
			EBootstrapState::LoadingDefaultDocument));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::LoadingDefaultDocument,
			EBootstrapState::Ready));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::LoadingDefaultDocument,
			EBootstrapState::Failed));
		EXPECT_TRUE(IsValidBootstrapTransition(
			EBootstrapState::LoadingWorkspace,
			EBootstrapState::Failed));
	}

	TEST(FEditorBootstrapStateTests, RejectsSkippedBackwardAndTerminalTransitions)
	{
		constexpr std::array States{
			EBootstrapState::ConstructingShell,
			EBootstrapState::WaitingForFirstPresent,
			EBootstrapState::LoadingWorkspace,
			EBootstrapState::WorkspaceReady,
			EBootstrapState::LoadingDefaultDocument,
			EBootstrapState::Ready,
			EBootstrapState::Failed,
		};
		for (const EBootstrapState From : States)
		{
			for (const EBootstrapState To : States)
			{
				const bool bExpected =
					(From == EBootstrapState::ConstructingShell
						&& To == EBootstrapState::WaitingForFirstPresent)
					|| (From == EBootstrapState::WaitingForFirstPresent
						&& (To == EBootstrapState::LoadingWorkspace
							|| To == EBootstrapState::Ready))
					|| (From == EBootstrapState::LoadingWorkspace
						&& (To == EBootstrapState::WorkspaceReady
							|| To == EBootstrapState::Failed))
					|| (From == EBootstrapState::WorkspaceReady
						&& To == EBootstrapState::LoadingDefaultDocument)
					|| (From == EBootstrapState::LoadingDefaultDocument
						&& (To == EBootstrapState::Ready
							|| To == EBootstrapState::Failed));
				EXPECT_EQ(IsValidBootstrapTransition(From, To), bExpected);
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
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::ConstructingShell), 1);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::WaitingForFirstPresent), 1);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::LoadingWorkspace), 2);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::WorkspaceReady), 2);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::LoadingDefaultDocument), 3);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::Ready), 3);
		EXPECT_EQ(GetBootstrapPhaseIndex(
			EBootstrapState::Failed), 0);
		EXPECT_EQ(GetBootstrapStepStatus(EBootstrapState::Ready),
			EBootstrapStepStatus::Ready);
		EXPECT_EQ(GetBootstrapStepStatus(EBootstrapState::Failed),
			EBootstrapStepStatus::Failed);
		EXPECT_EQ(GetBootstrapStepStatus(
			EBootstrapState::LoadingDefaultDocument),
			EBootstrapStepStatus::Pending);
	}

	TEST(FEditorBootstrapStateTests, StartupFramesExcludeEngineViewportRendering)
	{
		EXPECT_FALSE(ShouldRedrawEngineViewports(EEngineFrameMode::Startup));
		EXPECT_TRUE(ShouldRedrawEngineViewports(EEngineFrameMode::Running));
	}
}

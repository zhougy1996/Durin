#include "gtest/gtest.h"

#include "Editor/MainFrame/Public/Interfaces/IMainFrameModule.h"

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
						&& To == EEditorBootstrapState::Ready);
				EXPECT_EQ(IsValidEditorBootstrapTransition(From, To), bExpected);
			}
		}
	}
}

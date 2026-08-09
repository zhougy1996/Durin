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
						&& (To == EEditorBootstrapState::Ready
							|| To == EEditorBootstrapState::Failed));
				EXPECT_EQ(IsValidEditorBootstrapTransition(From, To), bExpected);
			}
		}
	}
}

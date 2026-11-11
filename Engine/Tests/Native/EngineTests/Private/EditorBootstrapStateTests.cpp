#include "gtest/gtest.h"

#include "Editor/EditorStartup.h"

namespace Durin::EditorStartup
{
	TEST(FEditorBootstrapStateTests, ValidatesEveryStatePairAgainstDocumentedTransitions)
	{
		constexpr std::array States{
			EBootstrapState::InitializingProject,
			EBootstrapState::LoadingWorkspace,
			EBootstrapState::Ready,
			EBootstrapState::Failed,
		};
		for (const EBootstrapState From : States)
		{
			for (const EBootstrapState To : States)
			{
				const bool bExpected =
					(From == EBootstrapState::InitializingProject
						&& (To == EBootstrapState::LoadingWorkspace || To == EBootstrapState::Ready || To == EBootstrapState::Failed))
					|| (From == EBootstrapState::LoadingWorkspace
						&& (To == EBootstrapState::Ready || To == EBootstrapState::Failed));
				EXPECT_EQ(IsValidBootstrapTransition(From, To), bExpected);
			}
		}
	}

	TEST(FEditorBootstrapStateTests, MapsTerminalAndPendingStatesToStepStatus)
	{
		EXPECT_EQ(GetBootstrapStepStatus(EBootstrapState::Ready),
			EHostStartupState::Ready);
		EXPECT_EQ(GetBootstrapStepStatus(EBootstrapState::Failed),
			EHostStartupState::Failed);
		EXPECT_EQ(GetBootstrapStepStatus(EBootstrapState::LoadingWorkspace),
			EHostStartupState::Pending);
	}

}

#include "Source/SharedSourceReplacementWorkflow.h"

#include <gtest/gtest.h>

namespace Durin::Editor::Texture
{
	namespace
	{
		struct FWorkflowHarness
		{
			bool bPending = true;
			bool bBuildSucceeded = true;
			bool bSaveSucceeded = true;
			bool bRestoreStarted = true;
			int32 PrepareCount = 0;
			int32 ReplacementBuildCount = 0;
			int32 SaveCount = 0;
			int32 CommitCount = 0;
			int32 RollbackCount = 0;
			int32 RestoreBuildCount = 0;

			auto MakeOperations() -> FSharedSourceReplacementWorkflow::FOperations
			{
				return {
					.Prepare = [this](std::string&) { ++PrepareCount; return true; },
					.StartReplacementBuild = [this](std::string&) {
						++ReplacementBuildCount;
						return true;
					},
					.IsBuildPending = [this] { return bPending; },
					.DidBuildSucceed = [this](std::string& OutError) {
						if (!bBuildSucceeded) OutError = "replacement build failed";
						return bBuildSucceeded;
					},
					.Save = [this](std::string& OutError) {
						++SaveCount;
						if (!bSaveSucceeded) OutError = "save failed";
						return bSaveSucceeded;
					},
					.Commit = [this] { ++CommitCount; },
					.Rollback = [this] { ++RollbackCount; },
					.StartRestoreBuild = [this](std::string& OutError) {
						++RestoreBuildCount;
						if (!bRestoreStarted) OutError = "restore submission failed";
						return bRestoreStarted;
					},
				};
			}
		};
	}

	TEST(SharedSourceReplacementWorkflowTests, PendingBuildDoesNotSaveOrCommit)
	{
		FWorkflowHarness Harness;
		FSharedSourceReplacementWorkflow Workflow;
		ASSERT_TRUE(Workflow.Begin(Harness.MakeOperations()));

		Workflow.Tick();

		EXPECT_TRUE(Workflow.IsBusy());
		EXPECT_EQ(Harness.SaveCount, 0);
		EXPECT_EQ(Harness.CommitCount, 0);
		EXPECT_EQ(Harness.RollbackCount, 0);
	}

	TEST(SharedSourceReplacementWorkflowTests, SuccessfulBuildSavesAndCommits)
	{
		FWorkflowHarness Harness;
		FSharedSourceReplacementWorkflow Workflow;
		ASSERT_TRUE(Workflow.Begin(Harness.MakeOperations()));
		Harness.bPending = false;

		Workflow.Tick();

		EXPECT_EQ(Workflow.GetPhase(), FSharedSourceReplacementWorkflow::EPhase::Succeeded);
		EXPECT_EQ(Harness.SaveCount, 1);
		EXPECT_EQ(Harness.CommitCount, 1);
		EXPECT_EQ(Harness.RollbackCount, 0);
	}

	TEST(SharedSourceReplacementWorkflowTests, FailedBuildRollsBackAndWaitsForRestore)
	{
		FWorkflowHarness Harness;
		FSharedSourceReplacementWorkflow Workflow;
		ASSERT_TRUE(Workflow.Begin(Harness.MakeOperations()));
		Harness.bPending = false;
		Harness.bBuildSucceeded = false;

		Workflow.Tick();

		EXPECT_EQ(Workflow.GetPhase(), FSharedSourceReplacementWorkflow::EPhase::RestoringOriginal);
		EXPECT_EQ(Harness.RollbackCount, 1);
		EXPECT_EQ(Harness.RestoreBuildCount, 1);
		EXPECT_EQ(Harness.SaveCount, 0);

		Harness.bBuildSucceeded = true;
		Workflow.Tick();
		EXPECT_EQ(Workflow.GetPhase(), FSharedSourceReplacementWorkflow::EPhase::Failed);
		EXPECT_EQ(Workflow.GetError(), "replacement build failed");
	}

	TEST(SharedSourceReplacementWorkflowTests, SaveFailurePreservesPrimaryAndRecoveryErrors)
	{
		FWorkflowHarness Harness;
		Harness.bSaveSucceeded = false;
		Harness.bRestoreStarted = false;
		FSharedSourceReplacementWorkflow Workflow;
		ASSERT_TRUE(Workflow.Begin(Harness.MakeOperations()));
		Harness.bPending = false;

		Workflow.Tick();

		EXPECT_EQ(Workflow.GetPhase(), FSharedSourceReplacementWorkflow::EPhase::Failed);
		EXPECT_EQ(Harness.RollbackCount, 1);
		EXPECT_EQ(Workflow.GetError(),
			"save failed Recovery also failed: restore submission failed");
	}

	TEST(SharedSourceReplacementWorkflowTests, AbortRollsBackPreparedReplacementOnce)
	{
		FWorkflowHarness Harness;
		FSharedSourceReplacementWorkflow Workflow;
		ASSERT_TRUE(Workflow.Begin(Harness.MakeOperations()));

		Workflow.Abort();
		Workflow.Abort();

		EXPECT_EQ(Workflow.GetPhase(), FSharedSourceReplacementWorkflow::EPhase::Failed);
		EXPECT_EQ(Harness.RollbackCount, 1);
	}
}

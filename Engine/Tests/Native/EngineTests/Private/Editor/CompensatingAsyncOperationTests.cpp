#include "Editor/CompensatingAsyncOperation.h"

#include <gtest/gtest.h>

namespace Durin::Editor
{
	namespace
	{
		struct FOperationHarness
		{
			FAsyncOperationCompletion ApplyCompletion;
			FAsyncOperationCompletion CompensationCompletion;
			bool bCommitSucceeded = true;
			bool bInlineApply = false;
			int32 PrepareCount = 0;
			int32 CommitCount = 0;
			int32 RollbackCount = 0;
			int32 CancelCount = 0;
			int32 FinishedCount = 0;
			bool bFinishedSuccessfully = false;
			std::string FinishedError;

			auto MakeOperations() -> FCompensatingAsyncOperation::FOperations
			{
				return {
					.Prepare = [this](std::string&) { ++PrepareCount; return true; },
					.StartApply = [this](
						FAsyncOperationCompletion Completion,
						FAsyncOperationCancel& OutCancel,
						std::string&) {
						OutCancel = [this] { ++CancelCount; };
						if (bInlineApply) Completion(true, {});
						else ApplyCompletion = std::move(Completion);
						return true;
					},
					.Commit = [this](std::string& OutError) {
						++CommitCount;
						if (!bCommitSucceeded) OutError = "commit failed";
						return bCommitSucceeded;
					},
					.Rollback = [this] { ++RollbackCount; },
					.StartCompensation = [this](
						FAsyncOperationCompletion Completion,
						FAsyncOperationCancel& OutCancel,
						std::string&) {
						OutCancel = [this] { ++CancelCount; };
						CompensationCompletion = std::move(Completion);
						return true;
					},
					.Finished = [this](bool bSucceeded, std::string_view Error) {
						++FinishedCount;
						bFinishedSuccessfully = bSucceeded;
						FinishedError = Error;
					},
				};
			}
		};
	}

	TEST(CompensatingAsyncOperationTests, SuccessfulApplyCommitsWithoutRollback)
	{
		FOperationHarness Harness;
		FCompensatingAsyncOperation Operation;
		ASSERT_TRUE(Operation.Begin(Harness.MakeOperations()));
		ASSERT_TRUE(Harness.ApplyCompletion);

		Harness.ApplyCompletion(true, {});

		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Succeeded);
		EXPECT_EQ(Harness.CommitCount, 1);
		EXPECT_EQ(Harness.RollbackCount, 0);
		EXPECT_TRUE(Harness.bFinishedSuccessfully);
	}

	TEST(CompensatingAsyncOperationTests, ApplyFailureRollsBackAndCompensates)
	{
		FOperationHarness Harness;
		FCompensatingAsyncOperation Operation;
		ASSERT_TRUE(Operation.Begin(Harness.MakeOperations()));

		Harness.ApplyCompletion(false, "apply failed");

		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Compensating);
		EXPECT_EQ(Harness.RollbackCount, 1);
		ASSERT_TRUE(Harness.CompensationCompletion);
		Harness.CompensationCompletion(true, {});
		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Failed);
		EXPECT_EQ(Operation.GetError(), "apply failed");
	}

	TEST(CompensatingAsyncOperationTests, CommitAndCompensationErrorsAreBothRetained)
	{
		FOperationHarness Harness;
		Harness.bCommitSucceeded = false;
		FCompensatingAsyncOperation Operation;
		ASSERT_TRUE(Operation.Begin(Harness.MakeOperations()));

		Harness.ApplyCompletion(true, {});
		ASSERT_TRUE(Harness.CompensationCompletion);
		Harness.CompensationCompletion(false, "restore failed");

		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Failed);
		EXPECT_EQ(Operation.GetError(),
			"commit failed Compensation also failed: restore failed");
		EXPECT_EQ(Harness.RollbackCount, 1);
	}

	TEST(CompensatingAsyncOperationTests, AbortCancelsAndRollsBackOnce)
	{
		FOperationHarness Harness;
		FCompensatingAsyncOperation Operation;
		ASSERT_TRUE(Operation.Begin(Harness.MakeOperations()));

		Operation.Abort();
		Operation.Abort();

		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Failed);
		EXPECT_EQ(Harness.CancelCount, 1);
		EXPECT_EQ(Harness.RollbackCount, 1);
		EXPECT_EQ(Harness.FinishedCount, 1);
	}

	TEST(CompensatingAsyncOperationTests, InlineApplyCompletionIsCommitted)
	{
		FOperationHarness Harness;
		Harness.bInlineApply = true;
		FCompensatingAsyncOperation Operation;

		ASSERT_TRUE(Operation.Begin(Harness.MakeOperations()));

		EXPECT_EQ(Operation.GetPhase(), FCompensatingAsyncOperation::EPhase::Succeeded);
		EXPECT_EQ(Harness.CommitCount, 1);
		EXPECT_EQ(Harness.CancelCount, 0);
	}
}

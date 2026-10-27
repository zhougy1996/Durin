#include "Misc/ScopedSlowTask.h"
#include <gtest/gtest.h>

using namespace Durin;

TEST(FSlowTaskTests, NestedScopesContributeToTheReservedParentFrame)
{
	FFeedbackContext Context;
	FScopedSlowTask Parent(100, "Import", Context);
	Parent.MakeDialog(true);
	Parent.EnterProgressFrame(40, "Read source");
	{
		FScopedSlowTask Child(4, "Decode", Context);
		Child.EnterProgressFrame();
		Child.EnterProgressFrame();
		ASSERT_TRUE(Parent.GetSnapshot()->Fraction);
		EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0.1);
		EXPECT_EQ(Context.GetVisibleTask()->Message, "Decode");
	}
	EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0.4);
	EXPECT_EQ(Context.GetVisibleTask()->Message, "Read source");
	Parent.EnterProgressFrame(60, "Build products");
	EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0.4);
	Parent.Complete();
	EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 1);
	EXPECT_FALSE(Context.GetVisibleTask());
}

TEST(FSlowTaskTests, DelayIsMeasuredFromTaskStartAndFastTasksNeverShow)
{
	double Now = 10;
	FFeedbackContext Context([&] { return Now; });
	FSlowTask Task(0, "Load", Context);
	Task.MakeDialogDelayed(1, true);
	Now = 10.9;
	EXPECT_FALSE(Context.GetVisibleTask());
	EXPECT_FALSE(Context.RequestCancel(Task.GetSnapshot()->Id));
	Now = 11;
	ASSERT_TRUE(Context.GetVisibleTask());
	EXPECT_DOUBLE_EQ(Context.GetVisibleTask()->ElapsedSeconds, 1);
	Task.Complete();
	Now = 20;
	EXPECT_FALSE(Context.GetVisibleTask());
	EXPECT_DOUBLE_EQ(Task.GetSnapshot()->ElapsedSeconds, 1);
	FSlowTask Fast(1, "Fast", Context);
	Fast.MakeDialogDelayed(1);
	Fast.Complete();
	Now = 100;
	EXPECT_FALSE(Context.GetVisibleTask());
}

TEST(FSlowTaskTests, UnknownChildProgressDoesNotInventAParentPercentage)
{
	FFeedbackContext Context;
	FScopedSlowTask Parent(100, "Import", Context);
	Parent.EnterProgressFrame(40);
	{
		FScopedSlowTask Child(0, "Prepare products", Context);
		EXPECT_FALSE(Parent.GetSnapshot()->Fraction);
		Child.SetProgress(0.5);
		EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0.2);
	}
	EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0.4);
}

TEST(FSlowTaskTests, CancellationIsCooperativeSharedAndIdempotent)
{
	FFeedbackContext Context;
	FScopedSlowTask Parent(1, "Import", Context);
	Parent.EnterProgressFrame();
	Parent.MakeDialog(true);
	FScopedSlowTask Child(0, "Decode", Context);
	const uint64 Id = Context.GetVisibleTask()->Id;
	EXPECT_TRUE(Context.RequestCancel(Id));
	EXPECT_FALSE(Context.RequestCancel(Id));
	EXPECT_TRUE(Parent.ShouldCancel());
	EXPECT_TRUE(Child.ShouldCancel());
	EXPECT_EQ(Parent.GetSnapshot()->Status, ESlowTaskStatus::Running);
	Parent.Cancel();
	EXPECT_EQ(Parent.GetSnapshot()->Status, ESlowTaskStatus::Canceled);
	EXPECT_FALSE(Context.GetVisibleTask());
}

TEST(FSlowTaskTests, UnknownProgressStaysUnknownAndMeasuredProgressIsClamped)
{
	FFeedbackContext Context;
	FSlowTask Task(0, "Load", Context);
	EXPECT_FALSE(Task.GetSnapshot()->Fraction);
	Task.SetProgress(2, "Measured");
	EXPECT_DOUBLE_EQ(*Task.GetSnapshot()->Fraction, 1);
	Task.SetProgress(-1);
	EXPECT_DOUBLE_EQ(*Task.GetSnapshot()->Fraction, 0);
	Task.SetProgress(std::numeric_limits<double>::quiet_NaN());
	EXPECT_FALSE(Task.GetSnapshot()->Fraction);
	Task.SetProgress(std::nullopt, "Waiting");
	EXPECT_FALSE(Task.GetSnapshot()->Fraction);
	EXPECT_EQ(Task.GetSnapshot()->Message, "Waiting");
}

TEST(FSlowTaskTests, TerminalResultCannotBeOverwritten)
{
	FFeedbackContext Context;
	FSlowTask Task(4, "Build", Context);
	Task.MakeDialog();
	EXPECT_FALSE(Context.RequestCancel(Task.GetSnapshot()->Id));
	Task.Fail("Invalid input");
	Task.Complete();
	Task.Cancel();
	Task.SetMessage("Overwritten");
	EXPECT_EQ(Task.GetSnapshot()->Status, ESlowTaskStatus::Failed);
	EXPECT_EQ(Task.GetSnapshot()->Diagnostic, "Invalid input");
	EXPECT_EQ(Task.GetSnapshot()->Message, "Build");
	EXPECT_FALSE(Context.GetVisibleTask());
}

TEST(FSlowTaskTests, IndependentAcrossFrameTasksDoNotBecomeImplicitParents)
{
	FFeedbackContext Context;
	FSlowTask First(0, "First", Context);
	First.MakeDialog(true);
	{
		FSlowTask Second(0, "Second", Context);
		Second.MakeDialog(true);
		EXPECT_EQ(Context.GetVisibleTask()->Title, "Second");
		EXPECT_TRUE(Context.RequestCancel(Second.GetSnapshot()->Id));
		EXPECT_FALSE(First.ShouldCancel());
	}
	EXPECT_EQ(Context.GetVisibleTask()->Title, "First");
}

TEST(FSlowTaskTests, RetiredContextInvalidatesSurvivingHandles)
{
	auto Context = std::make_unique<FFeedbackContext>();
	auto Task = std::make_unique<FSlowTask>(0, "Load", *Context);
	Task->MakeDialog(true);
	Context.reset();
	EXPECT_TRUE(Task->ShouldCancel());
	EXPECT_FALSE(Task->GetSnapshot());
	Task->SetProgress(0.5);
	Task->Complete();
	Task.reset();
}

TEST(FSlowTaskTests, ParentDestructionRetiresUnfinishedDescendants)
{
	FFeedbackContext Context;
	auto Parent = std::make_unique<FScopedSlowTask>(1, "Parent", Context);
	Parent->EnterProgressFrame();
	FSlowTask Child(0, "Child", Context);
	Child.MakeDialog(true);
	Parent.reset();
	EXPECT_FALSE(Child.GetSnapshot());
	EXPECT_TRUE(Child.ShouldCancel());
	EXPECT_FALSE(Context.GetVisibleTask());
}

TEST(FSlowTaskTests, ExceptionUnwindingDoesNotCompleteTheParentFrame)
{
	FFeedbackContext Context;
	FScopedSlowTask Parent(1, "Parent", Context);
	Parent.EnterProgressFrame();
	try
	{
		FScopedSlowTask Child(4, "Child", Context);
		Child.EnterProgressFrame();
		throw std::runtime_error("Expected test failure");
	}
	catch (const std::runtime_error&) {}
	EXPECT_DOUBLE_EQ(*Parent.GetSnapshot()->Fraction, 0);
	EXPECT_EQ(Parent.GetSnapshot()->Status, ESlowTaskStatus::Running);
}

TEST(FSlowTaskTests, WorkerScopesAndProgressDoNotCorruptOwnerScope)
{
	FFeedbackContext Context;
	FScopedSlowTask Owner(1, "Owner", Context);
	Owner.EnterProgressFrame();
	Owner.MakeDialog(true);
	std::atomic<bool> WorkerReady = false, Finish = false;
	std::thread Worker([&] {
		FScopedSlowTask Work(100, "Worker", Context);
		Work.MakeDialog(true);
		WorkerReady = true;
		for (int Index = 0; Index < 100; ++Index) Work.SetProgress(Index / 100.0);
		while (!Finish.load()) std::this_thread::yield();
		EXPECT_TRUE(Work.ShouldCancel());
	});
	while (!WorkerReady.load()) std::this_thread::yield();
	const auto Visible = Context.GetVisibleTask();
	EXPECT_TRUE(Visible);
	if (Visible) EXPECT_TRUE(Context.RequestCancel(Visible->Id));
	EXPECT_FALSE(Owner.ShouldCancel());
	EXPECT_DOUBLE_EQ(*Owner.GetSnapshot()->Fraction, 0);
	Finish = true;
	Worker.join();
	EXPECT_EQ(Context.GetVisibleTask()->Title, "Owner");
}

TEST(FSlowTaskTests, DialogRequestCanSwitchBetweenImmediateAndDelayedPresentation)
{
	double Now = 0;
	FFeedbackContext Context([&] { return Now; });
	FSlowTask Task(0, "Loading", Context);
	EXPECT_FALSE(Context.GetVisibleTask());
	Task.MakeDialogDelayed(5, true);
	Now = 1;
	EXPECT_FALSE(Context.GetVisibleTask());
	Task.MakeDialog(true);
	ASSERT_TRUE(Context.GetVisibleTask());
	Task.MakeDialogDelayed(5, true);
	EXPECT_FALSE(Context.GetVisibleTask());
	Now = 5;
	ASSERT_TRUE(Context.GetVisibleTask());
	EXPECT_TRUE(Context.RequestCancel(Task.GetSnapshot()->Id));
}

TEST(FSlowTaskTests, FinishedFeedbackDoesNotReadTheClockAgain)
{
	bool bFinished = false;
	FFeedbackContext Context([&] {
		EXPECT_FALSE(bFinished) << "Terminal feedback must use its frozen elapsed time.";
		return 1.0;
	});
	FSlowTask Task(1, "Build", Context);
	Task.MakeDialog(true);
	Task.Complete();
	bFinished = true;
	EXPECT_DOUBLE_EQ(Task.GetSnapshot()->ElapsedSeconds, 0);
	EXPECT_FALSE(Context.GetVisibleTask());
	EXPECT_FALSE(Context.RequestCancel(Task.GetSnapshot()->Id));
}

TEST(FSlowTaskTests, DelayedNewerTaskDoesNotHideAnOlderEligibleTask)
{
	double Now = 0;
	FFeedbackContext Context([&] { return Now; });
	FSlowTask First(0, "First", Context);
	First.MakeDialog();
	FSlowTask Second(0, "Second", Context);
	Second.MakeDialogDelayed(2);
	ASSERT_TRUE(Context.GetVisibleTask());
	EXPECT_EQ(Context.GetVisibleTask()->Title, "First");
	Now = 2;
	EXPECT_EQ(Context.GetVisibleTask()->Title, "Second");
	Second.Complete();
	EXPECT_EQ(Context.GetVisibleTask()->Title, "First");
	First.Complete();
	EXPECT_FALSE(Context.GetVisibleTask());
}

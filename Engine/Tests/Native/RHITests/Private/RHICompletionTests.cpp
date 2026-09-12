#include "RHICompletion.h"
#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHICompletionTests, BatchPreflightRequiresAnOwnedOrderedPendingPrefixWithoutMutation)
	{
		const auto Generation = AllocateRHIDeviceGeneration();
		FRHIGPUQueueTimeline Queue(Generation, {0}), Impostor(Generation, {0});
		const auto First = Queue.Reserve(), Hole = Queue.Reserve(), Last = Queue.Reserve();
		const auto Foreign = Impostor.Reserve();
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{First, Last}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{Hole, First, Last}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{First, First}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{Foreign}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{FRHIGPUSubmissionTicket{}}));
		EXPECT_TRUE(Queue.CanSubmitBatch(std::array{First, Hole, Last}));
		EXPECT_TRUE(Queue.CanSubmitBatch(std::array{First}));
		EXPECT_EQ(First.GetState(), ERHIGPUSubmissionState::Pending);
		ASSERT_TRUE(Queue.Cancel(Hole));
		EXPECT_TRUE(Queue.CanSubmitBatch(std::array{First, Last}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{First, Hole, Last}));
		ASSERT_TRUE(Queue.MarkSubmitted(First));
		EXPECT_TRUE(Queue.CanSubmitBatch(std::array{Last}));
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{First, Last}));
		Queue.Fail();
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{Last}));
	}

	TEST(FRHICompletionTests, RecordedSignalTracksNativeAcceptanceAndFailure)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Signal = FRHIGPUSubmissionReceipt::CreatePending();
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(Signal.GetTicket().GetState(), ERHIGPUSubmissionState::Invalid);
		EXPECT_FALSE(Signal.Resolve({}));
		const auto Ticket = Queue.Reserve();
		ASSERT_TRUE(Signal.Resolve(Ticket));
		EXPECT_FALSE(Signal.Resolve(Ticket));
		Signal.CancelUnresolved();
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Pending);
		ASSERT_TRUE(Queue.MarkSubmitted(Ticket));
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Submitted);
		Queue.Fail(true);
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::DeviceLost);
		EXPECT_FALSE(Signal.GetTicket().IsRetirementEligible());
	}

	TEST(FRHICompletionTests, CanceledRecordingCannotPublishANativePoint)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Signal = FRHIGPUSubmissionReceipt::CreatePending();
		Signal.CancelUnresolved();
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Canceled);
		EXPECT_FALSE(Signal.Resolve(Queue.Reserve()));
		EXPECT_EQ(Signal.GetTicket().GetState(), ERHIGPUSubmissionState::Invalid);
	}

	TEST(FRHICompletionTests, PendingWorkCannotCompleteOrRetire)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto First = Queue.Reserve();
		const auto Second = Queue.Reserve();
		EXPECT_FALSE(Queue.ObserveCompleted(First));
		EXPECT_FALSE(Queue.MarkSubmitted(Second));
		EXPECT_FALSE(First.IsRetirementEligible());
		EXPECT_TRUE(Queue.MarkSubmitted(First));
		EXPECT_FALSE(Queue.Cancel(First));
		EXPECT_TRUE(Queue.MarkSubmitted(Second));
		EXPECT_TRUE(Queue.ObserveCompleted(Second));
		EXPECT_FALSE(Second.IsRetirementEligible());
		EXPECT_TRUE(Queue.ObserveCompleted(First));
		EXPECT_TRUE(Second.IsRetirementEligible());
		EXPECT_EQ(Queue.GetPendingCount(), 0u);
	}

	TEST(FRHICompletionTests, CanceledMaximumDoesNotHideEarlierSubmittedUse)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto First = Queue.Reserve();
		const auto Second = Queue.Reserve();
		FRHIRetirementPrerequisites Uses;
		ASSERT_TRUE(Uses.Add(First));
		ASSERT_TRUE(Uses.Add(Second));
		ASSERT_EQ(Uses.GetTickets().size(), 1u);
		ASSERT_TRUE(Queue.MarkSubmitted(First));
		ASSERT_TRUE(Queue.Cancel(Second));
		EXPECT_EQ(Second.GetState(), ERHIGPUSubmissionState::Canceled);
		EXPECT_FALSE(Uses.IsRetirementEligible());
		ASSERT_TRUE(Queue.ObserveCompleted(First));
		EXPECT_TRUE(Uses.IsRetirementEligible());
		EXPECT_EQ(Second.GetState(), ERHIGPUSubmissionState::Canceled);
	}

	TEST(FRHICompletionTests, CanceledHolesDoNotSignalSuccessfulOutput)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Canceled = Queue.Reserve();
		const auto Next = Queue.Reserve();
		ASSERT_TRUE(Queue.Cancel(Canceled));
		ASSERT_TRUE(Queue.MarkSubmitted(Next));
		ASSERT_TRUE(Queue.ObserveCompleted(Next));
		EXPECT_TRUE(Canceled.IsRetirementEligible());
		EXPECT_EQ(Canceled.GetState(), ERHIGPUSubmissionState::Canceled);
		EXPECT_FALSE(Queue.ObserveCompleted(Canceled));
	}

	TEST(FRHICompletionTests, FasterGraphicsNeverRetiresCompute)
	{
		const auto Generation = AllocateRHIDeviceGeneration();
		FRHIGPUQueueTimeline Graphics(Generation, {0});
		FRHIGPUQueueTimeline Compute(Generation, {1});
		const auto Slow = Compute.Reserve();
		ASSERT_TRUE(Compute.MarkSubmitted(Slow));
		FRHIRetirementPrerequisites Uses;
		ASSERT_TRUE(Uses.Add(Slow));
		for (int Index = 0; Index < 20; ++Index)
		{
			const auto Fast = Graphics.Reserve();
			ASSERT_TRUE(Graphics.MarkSubmitted(Fast));
			ASSERT_TRUE(Graphics.ObserveCompleted(Fast));
			ASSERT_TRUE(Uses.Add(Fast));
		}
		EXPECT_EQ(Uses.GetTickets().size(), 2u);
		EXPECT_FALSE(Uses.IsRetirementEligible());
		ASSERT_TRUE(Compute.ObserveCompleted(Slow));
		EXPECT_TRUE(Uses.IsRetirementEligible());
	}

	TEST(FRHICompletionTests, ReplacementDeviceCannotCompleteOldTickets)
	{
		FRHIGPUQueueTimeline Old(AllocateRHIDeviceGeneration(), {0});
		FRHIGPUQueueTimeline Replacement(AllocateRHIDeviceGeneration(), {0});
		const auto OldUse = Old.Reserve();
		const auto NewUse = Replacement.Reserve();
		EXPECT_FALSE(Replacement.MarkSubmitted(OldUse));
		EXPECT_FALSE(Replacement.Cancel(OldUse));
		ASSERT_TRUE(Replacement.MarkSubmitted(NewUse));
		ASSERT_TRUE(Replacement.ObserveCompleted(NewUse));
		FRHIRetirementPrerequisites Uses;
		ASSERT_TRUE(Uses.Add(OldUse));
		ASSERT_TRUE(Uses.Add(NewUse));
		EXPECT_EQ(Uses.GetTickets().size(), 2u);
		EXPECT_FALSE(Uses.IsRetirementEligible());
	}

	TEST(FRHICompletionTests, FailureClosesAdmissionWithoutNormalRetirement)
	{
		for (bool bDeviceLost : {false, true})
		{
			FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
			const auto Submitted = Queue.Reserve();
			const auto Pending = Queue.Reserve();
			ASSERT_TRUE(Queue.MarkSubmitted(Submitted));
			Queue.Fail(bDeviceLost);
			const auto Expected = bDeviceLost ? ERHIGPUSubmissionState::DeviceLost
				: ERHIGPUSubmissionState::Failed;
			EXPECT_EQ(Submitted.GetState(), Expected);
			EXPECT_EQ(Pending.GetState(), Expected);
			EXPECT_EQ(Queue.Reserve().GetState(), ERHIGPUSubmissionState::Invalid);
			EXPECT_FALSE(Queue.ObserveCompleted(Submitted));
			EXPECT_FALSE(Submitted.IsRetirementEligible());
			EXPECT_FALSE(Pending.IsRetirementEligible());
		}
	}

	TEST(FRHICompletionTests, DuplicateQueueAuthorityCannotReplaceAnOutstandingUse)
	{
		const auto Generation = AllocateRHIDeviceGeneration();
		FRHIGPUQueueTimeline Original(Generation, {0}), Duplicate(Generation, {0});
		const auto Live = Original.Reserve();
		const auto First = Duplicate.Reserve();
		const auto Later = Duplicate.Reserve();
		ASSERT_TRUE(Duplicate.Cancel(First));
		ASSERT_TRUE(Duplicate.Cancel(Later));
		FRHIRetirementPrerequisites Uses;
		ASSERT_TRUE(Uses.Add(Live));
		EXPECT_FALSE(Uses.Add(Later));
		EXPECT_FALSE(Uses.IsRetirementEligible());
	}

	TEST(FRHICompletionTests, MetadataOutlivesBackendAndUnknownIsNotComplete)
	{
		FRHIGPUSubmissionTicket Done, Abandoned;
		{
			FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
			Done = Queue.Reserve();
			ASSERT_TRUE(Queue.MarkSubmitted(Done));
			ASSERT_TRUE(Queue.ObserveCompleted(Done));
			Abandoned = Queue.Reserve();
		}
		EXPECT_TRUE(Done.IsRetirementEligible());
		EXPECT_EQ(Abandoned.GetState(), ERHIGPUSubmissionState::Failed);
		EXPECT_FALSE(Abandoned.IsRetirementEligible());
		EXPECT_FALSE(FRHIGPUSubmissionTicket{}.IsRetirementEligible());
		FRHIRetirementPrerequisites Empty;
		EXPECT_TRUE(Empty.IsRetirementEligible());
		EXPECT_FALSE(Empty.Add({}));
	}
}

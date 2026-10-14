#include "RHICompletion.h"
#include "Backend/RHICompletionBackend.h"
#include "Backend/RHIDynamicBufferWrite.h"
#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHIDynamicBufferWriteTests, CPUAndRecordingOwnersPreventReuse)
	{
		FRHIDynamicBufferWritePool Pool;
		auto Memory = std::make_shared<std::array<std::byte, 8>>();
		Pool.Add(Memory, Memory->data());
		auto Lock = Pool.TryAcquire();
		ASSERT_TRUE(Lock);
		EXPECT_EQ(Lock->GetData(), Memory->data());
		EXPECT_FALSE(Pool.TryAcquire());
		auto Recording = Lock;
		Lock.reset();
		EXPECT_FALSE(Pool.TryAcquire());
		Recording.reset(); // Unreplayed/canceled locks have no GPU prerequisite.
		EXPECT_TRUE(Pool.TryAcquire());
	}

	TEST(FRHIDynamicBufferWriteTests, AllGPUUsesAndCurrentBindingsMustRetire)
	{
		FRHIDynamicBufferWritePool Pool;
		auto Memory = std::make_shared<std::array<std::byte, 8>>();
		Pool.Add(Memory, Memory->data());
		FRHIGPUQueueTimeline Graphics(AllocateRHIDeviceGeneration(), {0});
		FRHIGPUQueueTimeline Other(AllocateRHIDeviceGeneration(), {1});
		auto First = Graphics.Reserve(), Second = Other.Reserve();
		auto Range = Pool.TryAcquire();
		ASSERT_TRUE(Range);
		Range->AddGPUUse(First);
		Range->AddGPUUse(Second);
		Range.reset();
		EXPECT_FALSE(Pool.TryAcquire());
		ASSERT_TRUE(Graphics.MarkSubmitted(First));
		ASSERT_TRUE(Graphics.ObserveCompleted(First));
		EXPECT_FALSE(Pool.TryAcquire());
		ASSERT_TRUE(Other.MarkSubmitted(Second));
		ASSERT_TRUE(Other.ObserveCompleted(Second));
		auto CurrentBinding = Pool.TryAcquire();
		ASSERT_TRUE(CurrentBinding);
		EXPECT_FALSE(Pool.TryAcquire()); // Completion alone does not release bindings.
		CurrentBinding.reset();
		EXPECT_TRUE(Pool.TryAcquire());
	}

	TEST(FRHIDynamicBufferWriteTests, CanceledUsesRecycleButFailedGPUUsesDoNot)
	{
		for (uint32 Outcome : {0u, 1u, 2u})
		{
			FRHIDynamicBufferWritePool Pool;
			auto Memory = std::make_shared<std::array<std::byte, 8>>();
			Pool.Add(Memory, Memory->data());
			FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
			auto Signal = Queue.Reserve();
			auto Range = Pool.TryAcquire();
			ASSERT_TRUE(Range);
			Range->AddGPUUse(Signal);
			Range.reset();
			if (Outcome) Queue.Fail(Outcome == 2);
			else ASSERT_TRUE(Queue.Cancel(Signal));
			EXPECT_EQ(static_cast<bool>(Pool.TryAcquire()), Outcome == 0);
		}
	}

	TEST(FRHIDynamicBufferWriteTests, PoolExhaustionIsBoundedAndReleasedSlotsRecycle)
	{
		FRHIDynamicBufferWritePool Pool;
		std::array<std::shared_ptr<std::array<std::byte, 8>>, 3> Memory;
		for (auto& Bytes : Memory)
		{
			Bytes = std::make_shared<std::array<std::byte, 8>>();
			Pool.Add(Bytes, Bytes->data());
		}
		auto First = Pool.TryAcquire(), Second = Pool.TryAcquire(), Third = Pool.TryAcquire();
		ASSERT_TRUE(First && Second && Third);
		EXPECT_NE(First->GetData(), Second->GetData());
		EXPECT_NE(Second->GetData(), Third->GetData());
		EXPECT_FALSE(Pool.TryAcquire());
		auto* Released = Second->GetData();
		Second.reset();
		EXPECT_EQ(Pool.TryAcquire()->GetData(), Released);
	}

	TEST(FRHIDynamicBufferWriteTests, DiagnosticSnapshotCountsPreAcquisitionOccupancyWithoutChangingReuse)
	{
		FRHIDynamicBufferWritePool Pool;
		for (uint32 Index = 0; Index != 3; ++Index)
		{
			auto Memory = std::make_shared<std::array<std::byte, 8>>();
			Pool.Add(Memory, Memory->data());
		}
		auto Retained = Pool.TryAcquire(), InFlight = Pool.TryAcquire();
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Signal = Queue.Reserve();
		InFlight->AddGPUUse(Signal);
		InFlight.reset();
		FRHIDynamicBufferWritePoolSnapshot Snapshot;
		auto Acquired = Pool.TryAcquire(&Snapshot);
		ASSERT_TRUE(Acquired);
		EXPECT_EQ(Snapshot.RangeCount, 3u);
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 1u);
		EXPECT_EQ(Snapshot.ReusableRangeCount, 1u);
		EXPECT_FALSE(Pool.TryAcquire(&Snapshot));
		EXPECT_EQ(Snapshot.RetainedRangeCount, 2u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 1u);
		EXPECT_EQ(Snapshot.ReusableRangeCount, 0u);
		ASSERT_TRUE(Queue.MarkSubmitted(Signal));
		ASSERT_TRUE(Queue.ObserveCompleted(Signal));
		Acquired.reset();
		EXPECT_TRUE(Pool.TryAcquire(&Snapshot));
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 0u);
		EXPECT_EQ(Snapshot.ReusableRangeCount, 2u);
	}

	TEST(FRHICompletionTests, LogicalIdentitySurvivesCoalescingAndObserverRelease)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Signal = FRHIGPUSyncPoint::Create();
		const auto Observer = Signal;
		const auto Other = FRHIGPUSyncPoint::Create();
		EXPECT_NE(Signal, Other);
		EXPECT_EQ(Queue.GetPendingCount(), 0u);
		EXPECT_EQ(WaitForRHIGPUSyncPoint(Signal, 0), ERHIGPUWaitResult::Pending);
		const auto Producer = Queue.Reserve();
		ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Signal, Producer));
		ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Other, Producer));
		EXPECT_EQ(Signal, Observer);
		Signal = {};
		EXPECT_EQ(Observer.GetState(), ERHIGPUSubmissionState::Pending);
		ASSERT_TRUE(Queue.MarkSubmitted(Producer));
		EXPECT_FALSE(Observer->IsComplete());
		EXPECT_FALSE(FRHIGPUSyncPointBackend::Attach(FRHIGPUSyncPoint::Create(), Producer));
		ASSERT_TRUE(Queue.ObserveCompleted(Producer));
		EXPECT_TRUE(Observer->IsComplete());
		EXPECT_TRUE(Other->IsComplete());
		EXPECT_EQ(WaitForRHIGPUSyncPoint(Observer, 0), ERHIGPUWaitResult::Complete);
	}

	TEST(FRHICompletionTests, UnassociatedUsesStayExactAndCancellationNeverMakesOutputReady)
	{
		const auto Live = FRHIGPUSyncPoint::Create(), Canceled = FRHIGPUSyncPoint::Create();
		FRHIRetirementPrerequisites Uses;
		ASSERT_TRUE(Uses.Add(Live));
		ASSERT_TRUE(Uses.Add(Canceled));
		ASSERT_TRUE(Uses.Add(Live));
		EXPECT_EQ(Uses.GetSyncPoints().size(), 2u);
		FRHIGPUSyncPointBackend::CancelUnassociated(Canceled);
		EXPECT_FALSE(Canceled->IsComplete());
		EXPECT_TRUE(Canceled.IsRetirementEligible());
		EXPECT_FALSE(Uses.IsRetirementEligible());
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Producer = Queue.Reserve();
		ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Live, Producer));
		ASSERT_TRUE(Queue.MarkSubmitted(Producer));
		ASSERT_TRUE(Queue.ObserveCompleted(Producer));
		EXPECT_TRUE(Uses.IsRetirementEligible());
		EXPECT_EQ(WaitForRHIGPUSyncPoint(Canceled, 0), ERHIGPUWaitResult::Canceled);
	}

	TEST(FRHICompletionTests, RetainedLogicalMetadataSurvivesShutdownAndReleasesReservationStorage)
	{
		FRHIGPUSyncPointRef Observer;
		std::weak_ptr<FRHIGPUReservationState> Storage;
		{
			FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
			for (int Index = 0; Index != 2048; ++Index)
			{
				const auto Producer = Queue.Reserve();
				Observer = FRHIGPUSyncPoint::Create();
				ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Observer, Producer));
				if (Index % 2) ASSERT_TRUE(Queue.Cancel(Producer));
				else
				{
					ASSERT_TRUE(Queue.MarkSubmitted(Producer));
					ASSERT_TRUE(Queue.ObserveCompleted(Producer));
				}
				EXPECT_EQ(Queue.GetPendingCount(), 0u);
				Storage = FRHIGPUSyncPointBackend::GetReservation(Observer);
				Observer = {};
			}
			EXPECT_TRUE(Storage.expired());
			const auto Abandoned = Queue.Reserve();
			Observer = FRHIGPUSyncPoint::Create();
			ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Observer, Abandoned));
			Storage = FRHIGPUSyncPointBackend::GetReservation(Observer);
		}
		EXPECT_FALSE(Observer->IsComplete());
		EXPECT_EQ(WaitForRHIGPUSyncPoint(Observer, 0), ERHIGPUWaitResult::Failed);
		EXPECT_FALSE(Observer.IsRetirementEligible());
		Observer = {};
		EXPECT_TRUE(Storage.expired());
		EXPECT_EQ(WaitForRHIGPUSyncPoint({}, 0), ERHIGPUWaitResult::Invalid);
	}

	TEST(FRHICompletionTests, AssociationPublishesThreadSafeMetadata)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Signal = FRHIGPUSyncPoint::Create();
		const auto Producer = Queue.Reserve();
		std::atomic<bool> Stop = false, Invalid = false;
		std::thread Reader([Signal, &Stop, &Invalid] {
			while (!Stop.load())
			{
				const auto State = Signal->GetState();
				if (State != ERHIGPUSubmissionState::Pending && State != ERHIGPUSubmissionState::Submitted
					&& State != ERHIGPUSubmissionState::Complete) Invalid.store(true);
			}
		});
		EXPECT_TRUE(FRHIGPUSyncPointBackend::Attach(Signal, Producer));
		EXPECT_TRUE(Queue.MarkSubmitted(Producer));
		EXPECT_TRUE(Queue.ObserveCompleted(Producer));
		Stop.store(true);
		Reader.join();
		EXPECT_FALSE(Invalid.load());
		EXPECT_TRUE(Signal->IsComplete());
	}

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
		EXPECT_FALSE(Queue.CanSubmitBatch(std::array{FRHIGPUSyncPointRef{}}));
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
		const auto Signal = FRHIGPUSyncPoint::Create();
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Signal).Value, 0u);
		EXPECT_FALSE(FRHIGPUSyncPointBackend::Attach(Signal, {}));
		const auto SyncPoint = Queue.Reserve();
		ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(Signal, SyncPoint));
		EXPECT_FALSE(FRHIGPUSyncPointBackend::Attach(Signal, SyncPoint));
		FRHIGPUSyncPointBackend::CancelUnassociated(Signal);
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Pending);
		ASSERT_TRUE(Queue.MarkSubmitted(SyncPoint));
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Submitted);
		Queue.Fail(true);
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::DeviceLost);
		EXPECT_FALSE(Signal.IsRetirementEligible());
	}

	TEST(FRHICompletionTests, CanceledRecordingCannotPublishANativePoint)
	{
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		const auto Signal = FRHIGPUSyncPoint::Create();
		FRHIGPUSyncPointBackend::CancelUnassociated(Signal);
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Canceled);
		EXPECT_FALSE(FRHIGPUSyncPointBackend::Attach(Signal, Queue.Reserve()));
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Signal).Value, 0u);
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
		ASSERT_EQ(Uses.GetSyncPoints().size(), 2u);
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
		EXPECT_EQ(Uses.GetSyncPoints().size(), 2u);
		EXPECT_FALSE(Uses.IsRetirementEligible());
		ASSERT_TRUE(Compute.ObserveCompleted(Slow));
		EXPECT_TRUE(Uses.IsRetirementEligible());
	}

	TEST(FRHICompletionTests, ReplacementDeviceCannotCompleteOldSyncPoints)
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
		EXPECT_EQ(Uses.GetSyncPoints().size(), 2u);
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
		FRHIGPUSyncPointRef Done, Abandoned;
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
		EXPECT_FALSE(FRHIGPUSyncPointRef{}.IsRetirementEligible());
		FRHIRetirementPrerequisites Empty;
		EXPECT_TRUE(Empty.IsRetirementEligible());
		EXPECT_FALSE(Empty.Add({}));
	}
}

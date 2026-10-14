#include "RHICompletion.h"
#include "Backend/RHICompletionBackend.h"
#include "Backend/RHIDynamicBufferPagePool.h"
#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHIDynamicBufferPageTests, SplitPreservesExistingSpanThenPrefixAndSuffixOrder)
	{
		FRHIDynamicBufferPagePool Pool(1, 64);
		auto Memory = std::make_shared<std::array<std::byte, 64>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 64));
		auto First = Pool.TryAcquire(3, 1), Hole = Pool.TryAcquire(13, 1), Last = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(First && Hole && Last);
		Hole.reset();
		auto Aligned = Pool.TryAcquire(1, 4);
		ASSERT_TRUE(Aligned);
		EXPECT_EQ(Aligned->GetOffset(), 4u);
		auto Existing = Pool.TryAcquire(32, 1);
		ASSERT_TRUE(Existing);
		EXPECT_EQ(Existing->GetOffset(), 24u);
		auto Prefix = Pool.TryAcquire(1, 1), Suffix = Pool.TryAcquire(8, 1), Tail = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(Prefix && Suffix && Tail);
		EXPECT_EQ(Prefix->GetOffset(), 3u);
		EXPECT_EQ(Suffix->GetOffset(), 8u);
		EXPECT_EQ(Tail->GetOffset(), 56u);
		EXPECT_FALSE(Pool.TryAcquire(1, 1));
	}

	TEST(FRHIDynamicBufferPageTests, GrowthProbeReclaimsCompletedSlicesWithoutReservingSpace)
	{
		FRHIDynamicBufferPagePool Pool(1, 32);
		auto Memory = std::make_shared<std::array<std::byte, 32>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 32));
		auto InFlight = Pool.TryAcquire(16, 16), Retained = Pool.TryAcquire(16, 16);
		ASSERT_TRUE(InFlight && Retained);
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Signal = Queue.Reserve();
		InFlight->AddGPUUse(Signal);
		InFlight.reset();
		EXPECT_FALSE(Pool.CanAcquire(16, 16));
		ASSERT_TRUE(Queue.MarkSubmitted(Signal));
		ASSERT_TRUE(Queue.ObserveCompleted(Signal));
		EXPECT_FALSE(Pool.CanAcquire(UINT32_MAX, 16));
		EXPECT_TRUE(Pool.CanAcquire(16, 16));
		EXPECT_TRUE(Pool.CanAcquire(16, 16));
		FRHIDynamicBufferWritePoolSnapshot Snapshot;
		auto Reused = Pool.TryAcquire(16, 16, &Snapshot);
		ASSERT_TRUE(Reused);
		EXPECT_EQ(Reused->GetOffset(), 0u);
		EXPECT_EQ(Snapshot.RangeCount, 1u);
		EXPECT_EQ(Snapshot.FreeBytes, 16u);
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 0u);
		EXPECT_FALSE(Pool.CanAcquire(1, 1));
	}

	TEST(FRHIDynamicBufferPageTests, BuffersShareOnePageWithAlignedNonOverlappingSlices)
	{
		FRHIDynamicBufferPagePool Pool(2, 256);
		auto Memory = std::make_shared<std::array<std::byte, 128>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 128));
		auto First = Pool.TryAcquire(17, 16), Second = Pool.TryAcquire(9, 16);
		ASSERT_TRUE(First && Second);
		EXPECT_EQ(First->GetOffset(), 0u);
		EXPECT_EQ(First->GetSize(), 17u);
		EXPECT_EQ(Second->GetOffset(), 32u);
		EXPECT_EQ(Second->GetSize(), 9u);
		EXPECT_EQ(Second->GetData(), Memory->data() + 32);
		EXPECT_EQ(First->GetOwner(), Second->GetOwner());
		std::memset(First->GetData(), 1, First->GetSize());
		std::memset(Second->GetData(), 2, Second->GetSize());
		EXPECT_EQ((*Memory)[16], std::byte{1});
		EXPECT_EQ((*Memory)[32], std::byte{2});
	}

	TEST(FRHIDynamicBufferPageTests, ReferencedAndGPUInFlightSlicesCannotBeReused)
	{
		FRHIDynamicBufferPagePool Pool(1, 64);
		auto Memory = std::make_shared<std::array<std::byte, 64>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 64));
		auto First = Pool.TryAcquire(32, 16), Second = Pool.TryAcquire(32, 16);
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Signal = Queue.Reserve();
		First->AddGPUUse(Signal);
		First.reset();
		FRHIDynamicBufferWritePoolSnapshot Snapshot;
		EXPECT_FALSE(Pool.TryAcquire(16, 16, &Snapshot));
		EXPECT_EQ(Snapshot.PageCount, 1u);
		EXPECT_EQ(Snapshot.ReservedBytes, 64u);
		EXPECT_EQ(Snapshot.FreeBytes, 0u);
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 1u);
		ASSERT_TRUE(Queue.MarkSubmitted(Signal));
		ASSERT_TRUE(Queue.ObserveCompleted(Signal));
		auto Reused = Pool.TryAcquire(32, 16);
		ASSERT_TRUE(Reused);
		EXPECT_EQ(Reused->GetOffset(), 0u);
	}

	TEST(FRHIDynamicBufferPageTests, AdjacentReleasedSlicesMergeForLargerWrites)
	{
		FRHIDynamicBufferPagePool Pool(1, 64);
		auto Memory = std::make_shared<std::array<std::byte, 64>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 64));
		auto First = Pool.TryAcquire(16, 16), Second = Pool.TryAcquire(16, 16), Third = Pool.TryAcquire(32, 16);
		First.reset(); Second.reset();
		EXPECT_FALSE(Pool.TryAcquire(48, 16));
		auto Combined = Pool.TryAcquire(32, 16);
		ASSERT_TRUE(Combined);
		EXPECT_EQ(Combined->GetOffset(), 0u);
		Combined.reset(); Third.reset();
		EXPECT_TRUE(Pool.TryAcquire(64, 16));
	}

	TEST(FRHIDynamicBufferPageTests, PageAndByteLimitsRejectGrowthAndOversizedWrites)
	{
		FRHIDynamicBufferPagePool Pool(2, 96);
		auto Memory = std::make_shared<std::array<std::byte, 128>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 64));
		EXPECT_FALSE(Pool.CanAddPage(64));
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data() + 64, 32));
		EXPECT_FALSE(Pool.AddPage(Memory, Memory->data() + 96, 16));
		EXPECT_EQ(Pool.GetPageCount(), 2u);
		EXPECT_FALSE(Pool.TryAcquire(UINT32_MAX, 256));
		EXPECT_FALSE(Pool.TryAcquire(65, 16));
		EXPECT_TRUE(Pool.TryAcquire(64, 16));
		EXPECT_TRUE(Pool.TryAcquire(32, 16));
	}

	TEST(FRHIDynamicBufferPageTests, EveryQueueUseMustCompleteBeforeReclaim)
	{
		FRHIDynamicBufferPagePool Pool(1, 32);
		auto Memory = std::make_shared<std::array<std::byte, 32>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 32));
		FRHIGPUQueueTimeline FirstQueue(AllocateRHIDeviceGeneration(), {0}), SecondQueue(AllocateRHIDeviceGeneration(), {1});
		auto First = FirstQueue.Reserve(), Second = SecondQueue.Reserve();
		auto Range = Pool.TryAcquire(32, 16);
		Range->AddGPUUse(First); Range->AddGPUUse(Second); Range.reset();
		ASSERT_TRUE(FirstQueue.MarkSubmitted(First));
		ASSERT_TRUE(FirstQueue.ObserveCompleted(First));
		EXPECT_FALSE(Pool.TryAcquire(32, 16));
		ASSERT_TRUE(SecondQueue.Cancel(Second));
		EXPECT_TRUE(Pool.TryAcquire(32, 16));
	}

	TEST(FRHIDynamicBufferPageTests, ConcurrentRecordingReservesDistinctSlices)
	{
		FRHIDynamicBufferPagePool Pool(1, 128);
		auto Memory = std::make_shared<std::array<std::byte, 128>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 128));
		std::array<std::future<std::shared_ptr<FRHIDynamicBufferWriteRange>>, 4> Futures;
		for (auto& Future : Futures)
			Future = std::async(std::launch::async, [&] { return Pool.TryAcquire(32, 16); });
		std::array<std::shared_ptr<FRHIDynamicBufferWriteRange>, 4> Ranges;
		std::unordered_set<uint32> Offsets;
		for (size_t Index = 0; Index < Ranges.size(); ++Index)
		{
			Ranges[Index] = Futures[Index].get();
			ASSERT_TRUE(Ranges[Index]);
			Offsets.insert(Ranges[Index]->GetOffset());
		}
		EXPECT_EQ(Offsets.size(), 4u);
		EXPECT_FALSE(Pool.TryAcquire(1, 16));
	}

	TEST(FRHIDynamicBufferPageTests, FailedSubmissionsNeverAuthorizeNormalReuse)
	{
		FRHIDynamicBufferPagePool Pool(1, 32);
		auto Memory = std::make_shared<std::array<std::byte, 32>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 32));
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Range = Pool.TryAcquire(32, 16);
		Range->AddGPUUse(Queue.Reserve()); Range.reset(); Queue.Fail(true);
		EXPECT_FALSE(Pool.TryAcquire(32, 16));
	}

	TEST(FRHIDynamicBufferWriteTests, CPUAndRecordingOwnersPreventReuse)
	{
		FRHIDynamicBufferPagePool Pool(3, 24);
		auto Memory = std::make_shared<std::array<std::byte, 8>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 8));
		auto Lock = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(Lock);
		EXPECT_EQ(Lock->GetData(), Memory->data());
		EXPECT_FALSE(Pool.TryAcquire(8, 1));
		auto Recording = Lock;
		Lock.reset();
		EXPECT_FALSE(Pool.TryAcquire(8, 1));
		Recording.reset(); // Unreplayed/canceled locks have no GPU prerequisite.
		EXPECT_TRUE(Pool.TryAcquire(8, 1));
	}

	TEST(FRHIDynamicBufferWriteTests, AllGPUUsesAndCurrentBindingsMustRetire)
	{
		FRHIDynamicBufferPagePool Pool(3, 24);
		auto Memory = std::make_shared<std::array<std::byte, 8>>();
		ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 8));
		FRHIGPUQueueTimeline Graphics(AllocateRHIDeviceGeneration(), {0});
		FRHIGPUQueueTimeline Other(AllocateRHIDeviceGeneration(), {1});
		auto First = Graphics.Reserve(), Second = Other.Reserve();
		auto Range = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(Range);
		Range->AddGPUUse(First);
		Range->AddGPUUse(Second);
		Range.reset();
		EXPECT_FALSE(Pool.TryAcquire(8, 1));
		ASSERT_TRUE(Graphics.MarkSubmitted(First));
		ASSERT_TRUE(Graphics.ObserveCompleted(First));
		EXPECT_FALSE(Pool.TryAcquire(8, 1));
		ASSERT_TRUE(Other.MarkSubmitted(Second));
		ASSERT_TRUE(Other.ObserveCompleted(Second));
		auto CurrentBinding = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(CurrentBinding);
		EXPECT_FALSE(Pool.TryAcquire(8, 1)); // Completion alone does not release bindings.
		CurrentBinding.reset();
		EXPECT_TRUE(Pool.TryAcquire(8, 1));
	}

	TEST(FRHIDynamicBufferWriteTests, CanceledUsesRecycleButFailedGPUUsesDoNot)
	{
		for (uint32 Outcome : {0u, 1u, 2u})
		{
			FRHIDynamicBufferPagePool Pool(3, 24);
			auto Memory = std::make_shared<std::array<std::byte, 8>>();
			ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 8));
			FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
			auto Signal = Queue.Reserve();
			auto Range = Pool.TryAcquire(8, 1);
			ASSERT_TRUE(Range);
			Range->AddGPUUse(Signal);
			Range.reset();
			if (Outcome) Queue.Fail(Outcome == 2);
			else ASSERT_TRUE(Queue.Cancel(Signal));
			EXPECT_EQ(static_cast<bool>(Pool.TryAcquire(8, 1)), Outcome == 0);
		}
	}

	TEST(FRHIDynamicBufferWriteTests, PoolExhaustionIsBoundedAndReleasedSlicesRecycle)
	{
		FRHIDynamicBufferPagePool Pool(3, 24);
		std::array<std::shared_ptr<std::array<std::byte, 8>>, 3> Memory;
		for (auto& Bytes : Memory)
		{
			Bytes = std::make_shared<std::array<std::byte, 8>>();
			ASSERT_TRUE(Pool.AddPage(Bytes, Bytes->data(), 8));
		}
		auto First = Pool.TryAcquire(8, 1), Second = Pool.TryAcquire(8, 1), Third = Pool.TryAcquire(8, 1);
		ASSERT_TRUE(First && Second && Third);
		EXPECT_NE(First->GetData(), Second->GetData());
		EXPECT_NE(Second->GetData(), Third->GetData());
		EXPECT_FALSE(Pool.TryAcquire(8, 1));
		auto* Released = Second->GetData();
		Second.reset();
		EXPECT_EQ(Pool.TryAcquire(8, 1)->GetData(), Released);
	}

	TEST(FRHIDynamicBufferWriteTests, DiagnosticSnapshotCountsPreAcquisitionOccupancyWithoutChangingReuse)
	{
		FRHIDynamicBufferPagePool Pool(3, 24);
		for (uint32 Index = 0; Index != 3; ++Index)
		{
			auto Memory = std::make_shared<std::array<std::byte, 8>>();
			ASSERT_TRUE(Pool.AddPage(Memory, Memory->data(), 8));
		}
		auto Retained = Pool.TryAcquire(8, 1), InFlight = Pool.TryAcquire(8, 1);
		FRHIGPUQueueTimeline Queue(AllocateRHIDeviceGeneration(), {0});
		auto Signal = Queue.Reserve();
		InFlight->AddGPUUse(Signal);
		InFlight.reset();
		FRHIDynamicBufferWritePoolSnapshot Snapshot;
		auto Acquired = Pool.TryAcquire(8, 1, &Snapshot);
		ASSERT_TRUE(Acquired);
		EXPECT_EQ(Snapshot.RangeCount, 2u);
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 1u);
		EXPECT_EQ(Snapshot.FreeBytes, 8u);
		EXPECT_FALSE(Pool.TryAcquire(8, 1, &Snapshot));
		EXPECT_EQ(Snapshot.RetainedRangeCount, 2u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 1u);
		EXPECT_EQ(Snapshot.FreeBytes, 0u);
		ASSERT_TRUE(Queue.MarkSubmitted(Signal));
		ASSERT_TRUE(Queue.ObserveCompleted(Signal));
		Acquired.reset();
		EXPECT_TRUE(Pool.TryAcquire(8, 1, &Snapshot));
		EXPECT_EQ(Snapshot.RetainedRangeCount, 1u);
		EXPECT_EQ(Snapshot.AwaitingRetirementRangeCount, 0u);
		EXPECT_EQ(Snapshot.RangeCount, 1u);
		EXPECT_EQ(Snapshot.FreeBytes, 16u);
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

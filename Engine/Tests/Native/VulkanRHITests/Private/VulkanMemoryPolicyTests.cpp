#include <gtest/gtest.h>
#include "Backend/RHICompletionBackend.h"
#include "../../RDGTestAccess.h"
#include "RDG.h"

#include "PCH.VulkanRHI.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "VulkanCompletion.h"
#include "VulkanBuffer.h"
#include "VulkanDiagnostics.h"
#include "VulkanDescriptorSets.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanMemory.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"
#include "VulkanTransferArena.h"
#include "InlineRHITestScope.h"
#include "VulkanRHITestEnvironment.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto CheckNativeComputeWait(const char* Policy, bool bSameFamily, std::optional<bool> TransferSync2 = {}, bool bRHITransfer = false, bool bThreaded = false) -> void
		{
			struct FPolicyScope
			{
				std::string Previous = std::getenv("DURIN_VULKAN_COMPUTE_QUEUE")
					? std::getenv("DURIN_VULKAN_COMPUTE_QUEUE") : "";
				~FPolicyScope() { _putenv_s("DURIN_VULKAN_COMPUTE_QUEUE", Previous.c_str()); }
			} PolicyScope;
			_putenv_s("DURIN_VULKAN_COMPUTE_QUEUE", Policy);
			FInlineRHITestScope Scope;
			if (bThreaded) _putenv_s("DURIN_RHI_EXECUTION", "threaded");
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			const auto& Queues = GDynamicRHI->RHIGetQueueCapabilities();
			if (Queues.Compute == Queues.Graphics) GTEST_SKIP() << "Requested independent compute topology unavailable: " << Policy;
			ASSERT_EQ(Queues.Queues.size(), 2u);
			EXPECT_EQ(Queues.Queues[0].OwnershipDomain == Queues.Queues[1].OwnershipDomain, bSameFamily);
			if (bRHITransfer)
			{
				auto& Commands = FRHICommandListImmediate::Get();
				auto Texture = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("RHIQueueRoundTrip", 4, 4, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Texture);
				std::array<uint8, 64> Expected;
				for (uint32 Index = 0; Index < Expected.size(); ++Index) Expected[Index] = static_cast<uint8>(Index + 31);
				GDynamicRHI->RHIUpdateTexture2D(Commands, Texture, 0, 0,
					FUpdateTextureRegion2D(0, 0, 0, 0, 4, 4), 16, std::as_bytes(std::span{Expected}));
				FRHIQueueTransferDesc Forward{.Source = Queues.Graphics, .Destination = Queues.Compute,
					.Textures = {FRHITextureTransition::Whole(Texture.GetReference(), ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead)}};
				FRHIQueueTransferDesc Backward{.Source = Queues.Compute, .Destination = Queues.Graphics,
					.Textures = {FRHITextureTransition::Whole(Texture.GetReference(), ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead)}};
				auto ToCompute = GDynamicRHI->RHICreateQueueTransfer(Forward);
				auto ToGraphics = GDynamicRHI->RHICreateQueueTransfer(Backward);
				ASSERT_TRUE(ToCompute && ToGraphics);
				EXPECT_FALSE(GDynamicRHI->RHICreateQueueTransfer({}));
				Forward.Destination = Forward.Source;
				EXPECT_FALSE(GDynamicRHI->RHICreateQueueTransfer(Forward));
				const auto Release = Commands.BeginGPUSubmission({.Queue = Queues.Graphics});
				Commands.ReleaseQueueOwnership(ToCompute);
				Commands.EndGPUSubmission();
				const auto Compute = Commands.BeginGPUSubmission({.Queue = Queues.Compute, .Waits = {Release}});
				Commands.BeginDiagnosticRegion("ComputeHandoff");
				Commands.AcquireQueueOwnership(ToCompute);
				Commands.ReleaseQueueOwnership(ToGraphics);
				Commands.EndDiagnosticRegion();
				Commands.EndGPUSubmission();
				const auto Join = Commands.BeginGPUSubmission({.Queue = Queues.Graphics, .Waits = {Compute}});
				Commands.AcquireQueueOwnership(ToGraphics);
				Commands.EndGPUSubmission();
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::None);
				EXPECT_EQ(Release.GetState(), ERHIGPUSubmissionState::Pending);
				EXPECT_EQ(Compute.GetState(), ERHIGPUSubmissionState::Pending);
				EXPECT_EQ(Join.GetState(), ERHIGPUSubmissionState::Pending);
				EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Compute).Queue, Queues.Compute);
				EXPECT_GT(FRHIGPUSyncPointBackend::GetPoint(Join).Value, FRHIGPUSyncPointBackend::GetPoint(Release).Value);
				ToCompute.reset(); ToGraphics.reset();
				FByteBuffer Actual;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Texture, 0, 0, Actual));
				const auto Bytes = std::as_bytes(std::span{Expected});
				EXPECT_EQ(Actual, (FByteBuffer(Bytes.begin(), Bytes.end())));
				EXPECT_EQ(Join.GetState(), ERHIGPUSubmissionState::Complete);
				FRDGBuilder Graph;
				Graph.SetAsyncComputeEnabled(true);
				const auto Imported = Graph.RegisterExternalTexture(Texture, "Imported",
					ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
				for (uint32 Index = 0; Index < 3; ++Index)
				{
					const bool bCompute = Index != 1;
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Graph, std::to_string(Index),
						bCompute ? ERDGPassType::Compute : ERDGPassType::Graphics);
					if (bCompute) Graph.SetPassAsyncComputeEligible(Pass);
					FRDGBuilderTestAccessor::UseTexture(Graph, Pass, Imported,
						{ERHITextureAspect::Color, 0, 1, 0, 1}, ERDGUse::Read,
						bCompute ? ERHIAccess::ComputeShaderRead : ERHIAccess::GraphicsShaderRead);
				}
				const auto Recorded = Graph.Execute(Commands);
				ASSERT_TRUE(Recorded.has_value()) << FormatRDGError(Recorded);
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::None);
				ASSERT_EQ(FRDGBuilderTestAccessor::GetSubmissionSyncPoints(Graph).size(), 4u);
				for (uint32 Index = 0; Index < 4; ++Index)
				{
					EXPECT_EQ(FRDGBuilderTestAccessor::GetSubmissionSyncPoints(Graph)[Index].GetState(), ERHIGPUSubmissionState::Pending);
					EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(FRDGBuilderTestAccessor::GetSubmissionSyncPoints(Graph)[Index]).Queue,
						Index % 2 == 0 ? Queues.Compute : Queues.Graphics);
				}
				Actual.clear();
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Texture, 0, 0, Actual));
				EXPECT_EQ(Actual, (FByteBuffer(Bytes.begin(), Bytes.end())));
				for (const auto& LogicalSignal : FRDGBuilderTestAccessor::GetSubmissionSyncPoints(Graph))
					EXPECT_EQ(LogicalSignal.GetState(), ERHIGPUSubmissionState::Complete);
				return;
			}
			if (TransferSync2)
			{
				if (*TransferSync2 && !GDynamicRHI->RHIGetCapabilities()->bSupportsSynchronization2)
					GTEST_SKIP() << "Synchronization2 ownership barriers unavailable";
				const auto Result = RunVulkanQueueTransferForTesting(*TransferSync2);
				EXPECT_TRUE(Result.bBufferMatched);
				EXPECT_TRUE(Result.bTextureMatched);
				EXPECT_TRUE(Result.bRetainedUntilCompletion);
				EXPECT_TRUE(Result.bReleasedAfterCompletion);
				EXPECT_TRUE(Result.bUnselectedRangesPreserved);
				return;
			}
			const auto Result = RunVulkanCrossQueueWaitForTesting();
			EXPECT_TRUE(Result.bConsumerBlocked);
			EXPECT_TRUE(Result.bRetirementBlocked);
			EXPECT_TRUE(Result.bDescriptorReuseBlocked);
			EXPECT_TRUE(Result.bDescriptorReusedAfterCompletion);
			EXPECT_TRUE(Result.bTransferReuseBlocked);
			EXPECT_TRUE(Result.bTransferReusedAfterCompletion);
			EXPECT_TRUE(Result.bBatchOrdered);
			EXPECT_TRUE(Result.bBatchCycleRejected);
			EXPECT_TRUE(Result.bBatchMissingProducerRejected);
			EXPECT_TRUE(Result.bBatchMissingReservationRejected);
			EXPECT_TRUE(Result.bUniformReuseBlocked);
			EXPECT_TRUE(Result.bUniformReusedAfterCompletion);
			if (Result.bTimingSupported)
			{
				EXPECT_TRUE(Result.bTimingBlocked);
				EXPECT_TRUE(Result.bTimingCompleted);
			}
			EXPECT_TRUE(Result.bCompleted);
			EXPECT_NE(Result.Producer.Queue, Result.Consumer.Queue);
			EXPECT_EQ(Result.Producer.DeviceGeneration, Result.Consumer.DeviceGeneration);
		}
	}

	TEST(FVulkanCompletionIntegrationTests, SealingOwnsRecordingWithoutSubmittingAndDiscardCancels)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		const auto Result = TestVulkanSubmissionBoundary();
		EXPECT_TRUE(Result.bPayloadUsesExplicitSyncPoint);
		EXPECT_TRUE(Result.bLogicalSignalUsesRecordingSyncPoint);
		EXPECT_TRUE(Result.bQueuedStorageRetired);
		EXPECT_TRUE(Result.bQueuedStorageDiscarded);
		EXPECT_TRUE(Result.bSealDidNotSubmit);
		EXPECT_TRUE(Result.bEarlierSyncPointSubmitted);
		EXPECT_TRUE(Result.bDiscardCanceled);
		EXPECT_TRUE(Result.bStorageRetained);
		EXPECT_TRUE(Result.bStorageReleased);
		EXPECT_TRUE(Result.bAllocationRetained);
		EXPECT_TRUE(Result.bAllocationReturned);
		EXPECT_TRUE(Result.bTimingDiscarded);
	}

	TEST(FVulkanCompletionIntegrationTests, RetainedLogicalObserversDoNotAccumulateNativeFences)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		EXPECT_TRUE(TestVulkanSyncPointStorage());
	}

	TEST(FVulkanCompletionIntegrationTests, PartialNativeFailureQuarantinesOwnersUntilTeardown)
	{
		for (bool bDeviceLost : {false, true})
		{
			FVulkanSyncPointFailureTestResult Result;
			const auto Expected = bDeviceLost ? ERHIGPUSubmissionState::DeviceLost : ERHIGPUSubmissionState::Failed;
			{
				FInlineRHITestScope Scope;
				ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
				Result = TestVulkanSyncPointFailure(bDeviceLost);
				EXPECT_TRUE(Result.bRejectedCanceledProducer);
				EXPECT_TRUE(Result.bQuarantinedAcceptedPrefix);
				for (const auto& Signal : Result.Signals)
				{
					EXPECT_EQ(Signal.GetState(), Expected);
					EXPECT_FALSE(Signal->IsComplete());
					EXPECT_FALSE(Signal.IsRetirementEligible());
				}
			}
			for (size_t Index = 0; Index != 3; ++Index)
			{
				EXPECT_TRUE(Result.Owners[Index].expired());
				EXPECT_EQ(Result.Signals[Index].GetState(), Expected);
				EXPECT_EQ(WaitForRHIGPUSyncPoint(Result.Signals[Index], 0),
					bDeviceLost ? ERHIGPUWaitResult::DeviceLost : ERHIGPUWaitResult::Failed);
			}
		}
	}

	TEST(FVulkanCompletionIntegrationTests, SameFamilyComputeQueueWaitsForNativeTimelineSignal)
	{ CheckNativeComputeWait("same-family", true); }
	TEST(FVulkanCompletionIntegrationTests, DedicatedComputeFamilyWaitsForNativeTimelineSignal)
	{ CheckNativeComputeWait("dedicated", false); }
	TEST(FVulkanCompletionIntegrationTests, SharedRHIQueueTransferRoundTripsOnSameFamily)
	{ CheckNativeComputeWait("same-family", true, {}, true); }
	TEST(FVulkanCompletionIntegrationTests, SharedRHIQueueTransferRoundTripsOnDedicatedFamily)
	{ CheckNativeComputeWait("dedicated", false, {}, true); }
	TEST(FVulkanCompletionIntegrationTests, ThreadedSubmissionScopesRoundTripOnSameFamily)
	{ CheckNativeComputeWait("same-family", true, {}, true, true); }
	TEST(FVulkanCompletionIntegrationTests, ThreadedSubmissionScopesRoundTripOnDedicatedFamily)
	{ CheckNativeComputeWait("dedicated", false, {}, true, true); }
	TEST(FVulkanCompletionIntegrationTests, SameFamilyOwnershipReadbackMatchesWithSynchronization2)
	{ CheckNativeComputeWait("same-family", true, true); }
	TEST(FVulkanCompletionIntegrationTests, DedicatedFamilyOwnershipReadbackMatchesWithSynchronization2)
	{ CheckNativeComputeWait("dedicated", false, true); }
	TEST(FVulkanCompletionIntegrationTests, SameFamilyOwnershipReadbackMatchesWithLegacyBarriers)
	{ CheckNativeComputeWait("same-family", true, false); }
	TEST(FVulkanCompletionIntegrationTests, DedicatedFamilyOwnershipReadbackMatchesWithLegacyBarriers)
	{ CheckNativeComputeWait("dedicated", false, false); }

	TEST(FVulkanDescriptorPoolGrowthTests,
		ScalesEveryDescriptorTypeWithAllocationCapacity)
	{
		FVulkanDescriptorRequirements Requirements{
			.MaxSets = 1,
			.DescriptorCounts = {
				{vk::DescriptorType::eSampledImage, 1},
				{vk::DescriptorType::eSampler, 2}}};

		const FVulkanDescriptorRequirements Scaled =
			Requirements.ScaleToSetCapacity(512);

		EXPECT_EQ(Scaled.MaxSets, 512u);
		EXPECT_EQ(Scaled.DescriptorCounts.at(
			vk::DescriptorType::eSampledImage), 512u);
		EXPECT_EQ(Scaled.DescriptorCounts.at(
			vk::DescriptorType::eSampler), 1024u);
	}

	TEST(FVulkanDescriptorPoolGrowthTests,
		RoundsToWholeAllocationsAndBoundsGeometricGrowth)
	{
		FVulkanDescriptorRequirements Requirements{
			.MaxSets = 3,
			.DescriptorCounts = {{vk::DescriptorType::eUniformBuffer, 5}}};

		const FVulkanDescriptorRequirements Scaled =
			Requirements.ScaleToSetCapacity(512);

		EXPECT_EQ(Scaled.MaxSets, 513u);
		EXPECT_EQ(Scaled.DescriptorCounts.at(
			vk::DescriptorType::eUniformBuffer), 855u);
		EXPECT_EQ(GetNextDescriptorPoolSetCapacity(512), 1024u);
		EXPECT_EQ(GetNextDescriptorPoolSetCapacity(
			MaxDescriptorPoolSetCapacity), MaxDescriptorPoolSetCapacity);
	}

	TEST(FVulkanCompletionWatermarkTests,
		AdvancesOnlyAcrossContiguousObservedTokens)
	{
		FRHIGPUQueueTimeline Timeline(AllocateRHIDeviceGeneration(), {0});
		const auto First = Timeline.Reserve();
		const auto Second = Timeline.Reserve();
		const auto Third = Timeline.Reserve();
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(First).Value, 1u);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Second).Value, 2u);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Third).Value, 3u);
		ASSERT_TRUE(Timeline.MarkSubmitted(First));
		ASSERT_TRUE(Timeline.MarkSubmitted(Second));
		ASSERT_TRUE(Timeline.MarkSubmitted(Third));
		EXPECT_FALSE(First.IsRetirementEligible());
		ASSERT_TRUE(Timeline.ObserveCompleted(Second));
		EXPECT_FALSE(Second.IsRetirementEligible());
		ASSERT_TRUE(Timeline.ObserveCompleted(First));
		EXPECT_TRUE(First.IsRetirementEligible());
		EXPECT_TRUE(Second.IsRetirementEligible());
		EXPECT_FALSE(Third.IsRetirementEligible());
		ASSERT_TRUE(Timeline.ObserveCompleted(Third));
		EXPECT_TRUE(Third.IsRetirementEligible());
	}

	TEST(FVulkanMemoryBaselineTrackerTests,
		SaturatesCountersAndResetsWithoutClearingLiveGauges)
	{
		EXPECT_EQ(FVulkanMemoryBaselineTracker::SaturatingAdd(
			std::numeric_limits<uint64>::max() - 2, 3),
			std::numeric_limits<uint64>::max());

		FVulkanMemoryBaselineTracker Tracker;
		Tracker.RecordDescriptorPoolCreated(256);
		Tracker.RecordDescriptorSetsAllocated(3);
		Tracker.RecordDeferredDeleteEnqueued();
		const std::array<uint64, 2> HeapUsage{1024, 2048};
		const std::array<uint64, 2> HeapBudget{4096, 8192};
		Tracker.RecordHeapBudgets(HeapUsage, HeapBudget);
		Tracker.RecordAllocation(EVulkanAllocationClassCandidate::DeviceLocal,
			64, 128, false);
		Tracker.RecordUpload(32);
		Tracker.RecordReadback(16);
		Tracker.RecordFrameFenceWait(7);
		Tracker.RecordCommandBufferAllocation();
		Tracker.RecordCommandBufferReuse();

		Tracker.ResetCounters();
		const FVulkanMemoryBaselineStatistics Statistics = Tracker.Snapshot();
		EXPECT_EQ(Statistics.DescriptorPoolCount, 1u);
		EXPECT_EQ(Statistics.DescriptorPoolSetCapacity, 256u);
		EXPECT_EQ(Statistics.DescriptorAllocatedSetCount, 3u);
		EXPECT_EQ(Statistics.DeferredDeletePendingCount, 1u);
		EXPECT_EQ(Statistics.HeapCount, 2u);
		EXPECT_EQ(Statistics.HeapUsageBytes[1], 2048u);
		EXPECT_EQ(Statistics.HeapBudgetBytes[1], 8192u);
		EXPECT_EQ(Statistics.AllocationClasses[0].AllocationCount, 0u);
		EXPECT_EQ(Statistics.AllocationClasses[0].LiveAllocationCount, 1u);
		EXPECT_EQ(Statistics.AllocationClasses[0].LiveBytes, 128u);
		EXPECT_EQ(Statistics.AllocationClasses[0].PeakLiveBytes, 128u);
		EXPECT_EQ(Statistics.UploadOperationCount, 0u);
		EXPECT_EQ(Statistics.ReadbackOperationCount, 0u);
		EXPECT_EQ(Statistics.FrameFenceWaitCount, 0u);
		EXPECT_EQ(Statistics.CommandBufferAllocationCount, 0u);
		EXPECT_EQ(Statistics.CommandBufferReuseCount, 0u);
	}

	TEST(FVulkanMappedRangeTests, AlignsToAtomsAndClampsAtAllocationEnd)
	{
		EXPECT_EQ(NormalizeVulkanMappedRange(17, 1, 1024, 64).Offset, 0u);
		EXPECT_EQ(NormalizeVulkanMappedRange(17, 1, 1024, 64).Size, 64u);
		const FVulkanMappedRange Tail =
			NormalizeVulkanMappedRange(1000, 24, 1024, 64);
		EXPECT_EQ(Tail.Offset, 960u);
		EXPECT_EQ(Tail.Size, 64u);
		const FVulkanMappedRange Whole =
			NormalizeVulkanMappedRange(128, VK_WHOLE_SIZE, 1000, 64);
		EXPECT_EQ(Whole.Offset, 128u);
		EXPECT_EQ(Whole.Size, 872u);
	}

	TEST(FVulkanMappedRangeTests, FlushAndInvalidateFailuresAreNotIgnored)
	{
		FInlineRHITestScope Scope;
		struct FFailureResetScope
		{
			~FFailureResetScope() { ResetVulkanCreateFailures(); }
		} FailureReset;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(Commands,
			FRHIBufferCreateDesc::Create("MappedFailureBuffer", 256, 16,
				EBufferUsageFlags::Dynamic | EBufferUsageFlags::DestinationCopy
					| EBufferUsageFlags::KeepCPUAccessible));
		ASSERT_TRUE(Buffer);
		auto* VulkanBuffer = static_cast<FVulkanBuffer*>(Buffer.GetReference());

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::MappedMemoryFlush);
		EXPECT_THROW(VulkanBuffer->FlushMappedMemory(17, 1), std::runtime_error);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::MappedMemoryInvalidate);
		EXPECT_THROW(VulkanBuffer->InvalidateMappedMemory(17, 1), std::runtime_error);
	}

	TEST(FVulkanAllocationClassIntegrationTests,
		SelectsExplicitPropertiesAndPreservesLiveStatisticsAcrossReset)
	{
		FInlineRHITestScope Scope;

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		GDynamicRHI->RHIResetMemoryStatistics();
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		auto CreateBuffer = [&Commands](const char* Name, EBufferUsageFlags Usage) {
			return GDynamicRHI->RHICreateBuffer(Commands,
				FRHIBufferCreateDesc::Create(Name, 4096, 16, Usage));
		};
		FBufferRHIRef Static = CreateBuffer("ClassStatic",
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		FBufferRHIRef Dynamic = CreateBuffer("ClassDynamic",
			EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Dynamic);
		FBufferRHIRef Upload = CreateBuffer("ClassUpload",
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::SourceCopy);
		FBufferRHIRef Readback = CreateBuffer("ClassReadback",
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::DestinationCopy
				| EBufferUsageFlags::KeepCPUAccessible);
		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"ClassTexture", 8, 8, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource;
		FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(Commands, TextureDesc);
		ASSERT_TRUE(Static && Dynamic && Upload && Readback && Texture);

		auto* VulkanStatic = static_cast<FVulkanBuffer*>(Static.GetReference());
		auto* VulkanDynamic = static_cast<FVulkanBuffer*>(Dynamic.GetReference());
		auto* VulkanUpload = static_cast<FVulkanBuffer*>(Upload.GetReference());
		auto* VulkanReadback = static_cast<FVulkanBuffer*>(Readback.GetReference());
		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture.GetReference());
		EXPECT_EQ(VulkanStatic->GetAllocationClass(),
			EVulkanAllocationClassCandidate::DeviceLocal);
		EXPECT_TRUE(static_cast<bool>(VulkanStatic->GetMemoryPropertyFlags()
			& vk::MemoryPropertyFlagBits::eDeviceLocal));
		EXPECT_EQ(VulkanDynamic->GetAllocationClass(),
			EVulkanAllocationClassCandidate::DynamicUpload);
		EXPECT_EQ(VulkanUpload->GetAllocationClass(),
			EVulkanAllocationClassCandidate::TransferUpload);
		EXPECT_EQ(VulkanReadback->GetAllocationClass(),
			EVulkanAllocationClassCandidate::TransferReadback);
		EXPECT_EQ(VulkanTexture->GetAllocationClass(),
			EVulkanAllocationClassCandidate::DeviceLocal);
		for (FVulkanBuffer* HostBuffer :
			{VulkanDynamic, VulkanUpload, VulkanReadback})
		{
			EXPECT_TRUE(static_cast<bool>(HostBuffer->GetMemoryPropertyFlags()
				& vk::MemoryPropertyFlagBits::eHostVisible));
			EXPECT_NE(HostBuffer->GetMappedPointer(), nullptr);
		}

		const FRHIMemoryStatistics BeforeReset =
			GDynamicRHI->RHIGetMemoryStatistics();
		EXPECT_GE(BeforeReset.Classes[static_cast<uint32>(
			ERHIMemoryAllocationClass::DeviceLocal)].LiveAllocationCount, 2u);
		EXPECT_GT(BeforeReset.HeapCount, 0u);
		GDynamicRHI->RHIResetMemoryStatistics();
		const FRHIMemoryStatistics AfterReset =
			GDynamicRHI->RHIGetMemoryStatistics();
		for (uint32 ClassIndex = 0; ClassIndex < AfterReset.Classes.size();
			++ClassIndex)
		{
			EXPECT_EQ(AfterReset.Classes[ClassIndex].LiveAllocationCount,
				BeforeReset.Classes[ClassIndex].LiveAllocationCount);
			EXPECT_EQ(AfterReset.Classes[ClassIndex].LiveBytes,
				BeforeReset.Classes[ClassIndex].LiveBytes);
			EXPECT_EQ(AfterReset.Classes[ClassIndex].AllocationCount, 0u);
			EXPECT_EQ(AfterReset.Classes[ClassIndex].PeakLiveBytes,
				AfterReset.Classes[ClassIndex].LiveBytes);
		}

		const std::array<EBufferUsageFlags, 4> FailureUsages{
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static,
			EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Dynamic,
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::SourceCopy,
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::DestinationCopy
				| EBufferUsageFlags::KeepCPUAccessible};
		for (uint32 ClassIndex = 0; ClassIndex < FailureUsages.size(); ++ClassIndex)
		{
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
			EXPECT_FALSE(CreateBuffer("InjectedClassFailure",
				FailureUsages[ClassIndex]));
			EXPECT_EQ(GDynamicRHI->RHIGetMemoryStatistics().Classes[ClassIndex]
				.AllocationFailureCount, 1u);
		}
	}

	TEST(FVulkanTransferArenaIntegrationTests,
		ReusesBoundedPagesHandlesFragmentationOversizeAndExactWaits)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		ASSERT_NE(Device, nullptr);

		// A small independent arena makes alignment, holes, cap exhaustion,
		// oversize fallback, cancellation, and allocation failure deterministic.
		auto& Recording = FRHICommandListImmediate::Get();
		const auto Signal = Recording.BeginGPUSubmission({.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Recording.EndGPUSubmission();
		Recording.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		const auto SyncPoint = Signal;
		{
			FVulkanTransferArena Arena(*Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::TransferUpload,
				.PageSize = 256,
				.MaxPageCount = 1,
				.DebugName = "TransferArenaUnitPage"});
			auto First = Arena.Acquire(48, 64, SyncPoint);
			auto Middle = Arena.Acquire(64, 64, SyncPoint);
			auto Tail = Arena.Acquire(64, 64, SyncPoint);
			ASSERT_TRUE(First.Range && Middle.Range && Tail.Range);
			EXPECT_EQ(First.Range.GetOffset(), 0u);
			EXPECT_EQ(Middle.Range.GetOffset(), 64u);
			EXPECT_EQ(Tail.Range.GetOffset(), 128u);
			Middle.Range = {};
			auto Fragmented = Arena.Acquire(96, 16, SyncPoint);
			EXPECT_FALSE(Fragmented.Range);
			EXPECT_EQ(Fragmented.WaitSyncPoint.GetState(), ERHIGPUSubmissionState::Invalid);
			auto ReusedHole = Arena.Acquire(64, 16, SyncPoint);
			ASSERT_TRUE(ReusedHole.Range);
			EXPECT_EQ(ReusedHole.Range.GetOffset(), 48u);
			auto Oversize = Arena.Acquire(320, 64, SyncPoint);
			ASSERT_TRUE(Oversize.Range);
			EXPECT_EQ(Oversize.Range.GetOffset(), 0u);
		}
		{
			FVulkanTransferArena Arena(*Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::TransferUpload,
				.PageSize = 128, .MaxPageCount = 1, .DebugName = "TransferLeaseRetention"});
			auto Allocation = Arena.Acquire(128, 16, SyncPoint);
			ASSERT_TRUE(Allocation.Range);
			auto Lease = Allocation.Range.GetAllocationOwner();
			Allocation.Range = {};
			Recording.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(GDynamicRHI->RHIWaitForCompletion(SyncPoint, 1'000'000'000), ERHIGPUWaitResult::Complete);
			// Completion alone cannot reclaim storage retained outside the submission.
			EXPECT_FALSE(Arena.Acquire(128, 16, SyncPoint).Range);
			Lease.reset();
			EXPECT_TRUE(Arena.Acquire(128, 16, SyncPoint).Range);
		}
		{
			FVulkanTransferArena FailingArena(*Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::TransferReadback,
				.PageSize = 128,
				.MaxPageCount = 1,
				.DebugName = "TransferArenaFailure"});
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
			auto Failed = FailingArena.Acquire(64, 16, SyncPoint);
			EXPECT_TRUE(Failed.bAllocationFailed);
			EXPECT_FALSE(Failed.Range);
		}

		GDynamicRHI->RHIResetMemoryStatistics();
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		constexpr uint32 PageSize = 8 * 1024 * 1024;
		Durin::FByteBuffer Bytes(PageSize, std::byte{0x5a});
		std::vector<FBufferRHIRef> Destinations;
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(Commands,
				FRHIBufferCreateDesc::Create("ArenaCapDestination", PageSize, 4,
					EBufferUsageFlags::Static | EBufferUsageFlags::DestinationCopy));
			ASSERT_TRUE(Buffer);
			Destinations.push_back(Buffer);
			Commands.WriteBuffer(Buffer.GetReference(), Bytes.data(), PageSize, 0);
		}
		FBufferRHIRef OversizeDestination = GDynamicRHI->RHICreateBuffer(Commands,
			FRHIBufferCreateDesc::Create("ArenaOversizeDestination", PageSize + 256,
				4, EBufferUsageFlags::Static | EBufferUsageFlags::DestinationCopy));
		ASSERT_TRUE(OversizeDestination);
		Bytes.resize(PageSize + 256, std::byte{0x7c});
		Commands.WriteBuffer(OversizeDestination.GetReference(), Bytes.data(),
			static_cast<uint32>(Bytes.size()), 0);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);
		WaitForAllVulkanSubmissionsForTesting();
		Device->GetUploadArena().ReclaimCompleted();

		const FRHIMemoryStatistics UploadStatistics =
			GDynamicRHI->RHIGetMemoryStatistics();
		const auto& Upload = UploadStatistics.Classes[
			static_cast<uint32>(ERHIMemoryAllocationClass::TransferUpload)];
		EXPECT_EQ(Upload.ArenaCapacityBytes, 32ull * 1024 * 1024);
		EXPECT_EQ(Upload.ArenaLiveBytes, 0u);
		EXPECT_GE(Upload.ArenaHighWaterBytes, 32ull * 1024 * 1024);
		EXPECT_GE(Upload.ArenaReuseCount, 1u);
		EXPECT_GE(Upload.ArenaOverflowCount, 1u);
		EXPECT_GE(Upload.ArenaOversizeCount, 1u);
		EXPECT_GE(Upload.ArenaWaitCount, 1u);
		GDynamicRHI->RHIResetMemoryStatistics();
		const FRHIMemoryStatistics ResetStatistics =
			GDynamicRHI->RHIGetMemoryStatistics();
		const auto& ResetUpload = ResetStatistics.Classes[
			static_cast<uint32>(ERHIMemoryAllocationClass::TransferUpload)];
		EXPECT_EQ(ResetUpload.ArenaCapacityBytes, Upload.ArenaCapacityBytes);
		EXPECT_EQ(ResetUpload.ArenaLiveBytes, 0u);
		EXPECT_EQ(ResetUpload.ArenaHighWaterBytes, 0u);
		EXPECT_EQ(ResetUpload.ArenaReuseCount, 0u);
		EXPECT_EQ(ResetUpload.ArenaOverflowCount, 0u);
		EXPECT_EQ(ResetUpload.ArenaOversizeCount, 0u);
		EXPECT_EQ(ResetUpload.ArenaWaitCount, 0u);

		const FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"ArenaReadbackTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::CPUReadback);
		FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(Commands, TextureDesc);
		ASSERT_TRUE(Texture);
		std::array<uint8, 64> Expected{};
		for (uint32 Index = 0; Index < Expected.size(); ++Index)
			Expected[Index] = static_cast<uint8>(Index + 17);
		GDynamicRHI->RHIUpdateTexture2D(Commands, Texture, 0, 0,
			FUpdateTextureRegion2D(0, 0, 0, 0, 4, 4), 16,
			std::as_bytes(std::span{Expected}));
		for (uint32 ReadIndex = 0; ReadIndex < 3; ++ReadIndex)
		{
			Durin::FByteBuffer Actual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				Commands, Texture, 0, 0, Actual));
			const Durin::FByteView ExpectedBytes =
				std::as_bytes(std::span{Expected});
			EXPECT_EQ(Actual, (Durin::FByteBuffer(
				ExpectedBytes.begin(), ExpectedBytes.end())));
		}
		const FRHIMemoryStatistics ReadbackStatistics =
			GDynamicRHI->RHIGetMemoryStatistics();
		const auto& Readback = ReadbackStatistics.Classes[
			static_cast<uint32>(ERHIMemoryAllocationClass::TransferReadback)];
		EXPECT_EQ(Readback.ArenaCapacityBytes, 4ull * 1024 * 1024);
		EXPECT_EQ(Readback.ArenaLiveBytes, 0u);
		EXPECT_GE(Readback.ArenaReuseCount, 2u);
	}

	TEST(FVulkanCompletionIntegrationTests, AsyncTextureReadbackPreservesPixelsAndCancelsWithoutGPUWait)
	{
		FInlineRHITestScope Scope;
		_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto& Commands = FRHICommandListImmediate::Get();
		auto Texture = GDynamicRHI->RHICreateTexture(Commands,
			FRHITextureCreateDesc::Create2D("AsyncReadback", 7, 3, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
		ASSERT_TRUE(Texture);
		FByteBuffer Expected(7 * 3 * 4, std::byte{0x39});
		GDynamicRHI->RHIUpdateTexture2D(Commands, Texture, 0, 0,
			FUpdateTextureRegion2D(0, 0, 0, 0, 7, 3), 28, Expected);
		auto First = Commands.EnqueueTextureReadback(Texture);
		EXPECT_EQ(First->GetState(), ERHITextureReadbackState::Pending);
		FByteBuffer Replacement(Expected.size(), std::byte{0x71});
		GDynamicRHI->RHIUpdateTexture2D(Commands, Texture, 0, 0,
			FUpdateTextureRegion2D(0, 0, 0, 0, 7, 3), 28, Replacement);
		auto Second = Commands.EnqueueTextureReadback(Texture);
		auto Canceled = Commands.EnqueueTextureReadback(Texture);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		Canceled->Cancel();
		Texture = nullptr; // Recorded GPU copies retain their source allocation.
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		do
		{
			Commands.PollTextureReadbacks();
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			if (First->GetState() != ERHITextureReadbackState::Pending
				&& Second->GetState() != ERHITextureReadbackState::Pending) break;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < Deadline);
		FByteBuffer Pixels;
		ASSERT_TRUE(First->TakePixels(Pixels));
		EXPECT_EQ(Pixels, Expected);
		EXPECT_FALSE(First->TakePixels(Pixels));
		ASSERT_TRUE(Second->TakePixels(Pixels));
		EXPECT_EQ(Pixels, Replacement);
		EXPECT_EQ(Canceled->GetState(), ERHITextureReadbackState::Canceled);
		EXPECT_FALSE(Canceled->TakePixels(Pixels));
		auto Invalid = Commands.EnqueueTextureReadback(nullptr);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(Invalid->GetState(), ERHITextureReadbackState::Failed);
	}

	TEST(FVulkanCompletionIntegrationTests, EarlyNativeRetirementDoesNotCancelAnOpenLogicalBatch)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto& Commands = FRHICommandListImmediate::Get();
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
		const auto Prefix = GetLastVulkanSyncPointForTesting();
		ASSERT_EQ(GDynamicRHI->RHIWaitForCompletion(Prefix, 1'000'000'000), ERHIGPUWaitResult::Complete);
		EXPECT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Signal).Value, 0u);
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Signal.GetState(), ERHIGPUSubmissionState::Submitted);
		EXPECT_GT(FRHIGPUSyncPointBackend::GetPoint(Signal).Value, FRHIGPUSyncPointBackend::GetPoint(Prefix).Value);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000), ERHIGPUWaitResult::Complete);
	}

	TEST(FVulkanCompletionIntegrationTests, LogicalBatchesCoalesceAcrossCPUReplaySegments)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto& Commands = FRHICommandListImmediate::Get();
		const auto Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics;
		const auto First = Commands.BeginGPUSubmission({.Queue = Queue});
		const auto Observer = First;
		EXPECT_EQ(WaitForRHIGPUSyncPoint(First, 1'000'000'000), ERHIGPUWaitResult::Pending);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(First, 0), ERHIGPUWaitResult::Pending);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(First).Value, 0u);
		Commands.EndGPUSubmission();
		const auto Second = Commands.BeginGPUSubmission({.Queue = Queue, .Waits = {First}});
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		ASSERT_EQ(First.GetState(), ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(First, Observer);
		EXPECT_NE(First, Second);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(Second), FRHIGPUSyncPointBackend::GetPoint(First));
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Second, 0), ERHIGPUWaitResult::Pending);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
		EXPECT_EQ(Second.GetState(), ERHIGPUSubmissionState::Submitted);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Second, 1'000'000'000), ERHIGPUWaitResult::Complete);
		EXPECT_EQ(First.GetState(), ERHIGPUSubmissionState::Complete);
		EXPECT_TRUE(Observer->IsComplete());
	}

	TEST(FVulkanCompletionIntegrationTests,
		SubmissionPublishesTokenAndRetirementWaitsForItsFence)
	{
		FInlineRHITestScope Scope;

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ResetVulkanMemoryBaselineStatistics();
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(Commands,
			FRHIBufferCreateDesc::Create("CompletionTokenBuffer", 256, 16,
				EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static));
		ASSERT_TRUE(Buffer);
		const std::array<uint8, 16> Bytes{};
		Commands.WriteBuffer(Buffer.GetReference(), Bytes.data(), Bytes.size(), 0);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		const auto Pending = GetLastVulkanSyncPointForTesting();
		EXPECT_EQ(Pending.GetState(), ERHIGPUSubmissionState::Pending);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Pending, 0), ERHIGPUWaitResult::Pending);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);

		const FVulkanCompletionTestStats Submitted =
			GetVulkanCompletionTestStats();
		EXPECT_GT(Submitted.LastSubmittedToken, 0u);
		EXPECT_EQ(Submitted.LastReservedToken, Submitted.LastSubmittedToken);
		EXPECT_GT(Submitted.PendingSubmissionCount, 0u);
		const auto SyncPoint = GetLastVulkanSyncPointForTesting();
		EXPECT_EQ(SyncPoint.GetState(), ERHIGPUSubmissionState::Submitted);
		EXPECT_EQ(FRHIGPUSyncPointBackend::GetPoint(SyncPoint).Value, Submitted.LastSubmittedToken);
		EXPECT_GT(FRHIGPUSyncPointBackend::GetPoint(SyncPoint).DeviceGeneration, 0u);
		const auto& Queues = GDynamicRHI->RHIGetQueueCapabilities();
		ASSERT_EQ(Queues.Queues.size(), 1u);
		EXPECT_EQ(Queues.Graphics, Queues.Compute);
		EXPECT_EQ(Queues.DeviceGeneration, FRHIGPUSyncPointBackend::GetPoint(SyncPoint).DeviceGeneration);
		EXPECT_EQ(GDynamicRHI->RHIGetCompletionStatus(SyncPoint), ERHIGPUSubmissionState::Submitted);
		EXPECT_EQ(GDynamicRHI->RHIGetCompletionStatus({}), ERHIGPUSubmissionState::Invalid);
		EXPECT_FALSE(SyncPoint.IsRetirementEligible());
		// The native payload now retains the RHI wrapper as well as its allocation.
		EXPECT_GT(Buffer->GetRefCount(), 1u);

		Buffer = nullptr;
		Commands.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		EXPECT_FALSE(SyncPoint.IsRetirementEligible());

		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(SyncPoint, 1'000'000'000), ERHIGPUWaitResult::Complete);
		ReleaseCompletedVulkanResourcesForTesting();
		const FVulkanCompletionTestStats Completed =
			GetVulkanCompletionTestStats();
		EXPECT_EQ(Completed.CompletedToken, Submitted.LastSubmittedToken);
		EXPECT_EQ(Completed.PendingSubmissionCount, 0u);
		EXPECT_EQ(SyncPoint.GetState(), ERHIGPUSubmissionState::Complete);
		EXPECT_TRUE(SyncPoint.IsRetirementEligible());
	}

	TEST(FVulkanCompletionIntegrationTests, MatchingCoordinatesCannotImpersonateAQueueAuthority)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		const auto& Queues = GDynamicRHI->RHIGetQueueCapabilities();
		FRHIGPUQueueTimeline Impostor(Queues.DeviceGeneration, Queues.Graphics);
		const auto SyncPoint = Impostor.Reserve();
		ASSERT_TRUE(Impostor.MarkSubmitted(SyncPoint));
		ASSERT_TRUE(Impostor.ObserveCompleted(SyncPoint));
		EXPECT_EQ(GDynamicRHI->RHIGetCompletionStatus(SyncPoint), ERHIGPUSubmissionState::Invalid);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(SyncPoint, 0), ERHIGPUWaitResult::Invalid);
		FRHIGPUQueueTimeline Unknown(Queues.DeviceGeneration, {UINT32_MAX});
		const auto UnknownSyncPoint = Unknown.Reserve();
		EXPECT_EQ(GDynamicRHI->RHIGetCompletionStatus(UnknownSyncPoint), ERHIGPUSubmissionState::Invalid);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(UnknownSyncPoint, 0), ERHIGPUWaitResult::Invalid);
	}

	TEST(FVulkanCompletionIntegrationTests, CompletedSyncPointCannotAddressAReplacementDevice)
	{
		FRHIGPUSyncPointRef Old;
		{
			FInlineRHITestScope Scope;
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			GDynamicRHI->RHIBeginFrame({.FrameNumber = 0});
			GDynamicRHI->RHIEndFrame();
			Old = GetLastVulkanSyncPointForTesting();
			EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Old, 1'000'000'000), ERHIGPUWaitResult::Complete);
		}
		EXPECT_EQ(Old.GetState(), ERHIGPUSubmissionState::Complete);
		EXPECT_EQ(WaitForRHIGPUSyncPoint(Old, 0), ERHIGPUWaitResult::Complete);
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		EXPECT_NE(FRHIGPUSyncPointBackend::GetPoint(Old).DeviceGeneration, GDynamicRHI->RHIGetQueueCapabilities().DeviceGeneration);
		EXPECT_EQ(GDynamicRHI->RHIGetCompletionStatus(Old), ERHIGPUSubmissionState::Invalid);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Old, 0), ERHIGPUWaitResult::Invalid);
	}

	TEST(FVulkanCompletionIntegrationTests,
		EmptyIrregularFramesAdvanceBySubmissionRatherThanFrameAge)
	{
		FInlineRHITestScope Scope;

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		const std::array<uint64, 5> FrameNumbers{0, 17, 2, 101, 4};
		uint64 PreviousSubmittedToken = 0;
		for (uint64 FrameNumber : FrameNumbers)
		{
			GDynamicRHI->RHIBeginFrame({.FrameNumber = FrameNumber});
			GDynamicRHI->RHIEndFrame();
			const FVulkanCompletionTestStats Statistics =
				GetVulkanCompletionTestStats();
			EXPECT_GT(Statistics.LastSubmittedToken, PreviousSubmittedToken);
			EXPECT_LE(Statistics.CompletedToken, Statistics.LastSubmittedToken);
			PreviousSubmittedToken = Statistics.LastSubmittedToken;
		}

		WaitForAllVulkanSubmissionsForTesting();
		const FVulkanCompletionTestStats Completed =
			GetVulkanCompletionTestStats();
		EXPECT_EQ(Completed.LastSubmittedToken, PreviousSubmittedToken);
		EXPECT_EQ(Completed.CompletedToken, PreviousSubmittedToken);
		EXPECT_EQ(Completed.PendingSubmissionCount, 0u);
	}
} // namespace Durin::VulkanRHI

#include <gtest/gtest.h>

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
			kMaxDescriptorPoolSetCapacity), kMaxDescriptorPoolSetCapacity);
	}

	TEST(FVulkanCompletionWatermarkTests,
		AdvancesOnlyAcrossContiguousObservedTokens)
	{
		FVulkanCompletionWatermark Watermark;
		const FVulkanCompletionToken First = Watermark.AllocateToken();
		const FVulkanCompletionToken Second = Watermark.AllocateToken();
		const FVulkanCompletionToken Third = Watermark.AllocateToken();

		EXPECT_EQ(First, 1u);
		EXPECT_EQ(Second, 2u);
		EXPECT_EQ(Third, 3u);
		EXPECT_TRUE(Watermark.IsRetirementEligible(0));
		EXPECT_FALSE(Watermark.IsRetirementEligible(First));

		Watermark.ObserveCompleted(Second);
		EXPECT_EQ(Watermark.GetCompletedToken(), 0u);
		EXPECT_FALSE(Watermark.IsRetirementEligible(Second));

		Watermark.ObserveCompleted(First);
		EXPECT_EQ(Watermark.GetCompletedToken(), Second);
		EXPECT_TRUE(Watermark.IsRetirementEligible(First));
		EXPECT_TRUE(Watermark.IsRetirementEligible(Second));
		EXPECT_FALSE(Watermark.IsRetirementEligible(Third));

		Watermark.ObserveCompleted(Third);
		EXPECT_EQ(Watermark.GetCompletedToken(), Third);
		EXPECT_TRUE(Watermark.IsRetirementEligible(Third));
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
		{
			FVulkanTransferArena Arena(*Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::TransferUpload,
				.PageSize = 256,
				.MaxPageCount = 1,
				.DebugName = "TransferArenaUnitPage"});
			auto First = Arena.Acquire(48, 64, 1);
			auto Middle = Arena.Acquire(64, 64, 1);
			auto Tail = Arena.Acquire(64, 64, 1);
			ASSERT_TRUE(First.Range && Middle.Range && Tail.Range);
			EXPECT_EQ(First.Range.GetOffset(), 0u);
			EXPECT_EQ(Middle.Range.GetOffset(), 64u);
			EXPECT_EQ(Tail.Range.GetOffset(), 128u);
			Middle.Range = {};
			auto Fragmented = Arena.Acquire(96, 16, 1);
			EXPECT_FALSE(Fragmented.Range);
			EXPECT_EQ(Fragmented.WaitToken, 0u);
			auto ReusedHole = Arena.Acquire(64, 16, 1);
			ASSERT_TRUE(ReusedHole.Range);
			EXPECT_EQ(ReusedHole.Range.GetOffset(), 48u);
			auto Oversize = Arena.Acquire(320, 64, 1);
			ASSERT_TRUE(Oversize.Range);
			EXPECT_EQ(Oversize.Range.GetOffset(), 0u);
		}
		{
			FVulkanTransferArena FailingArena(*Device, {
				.AllocationClass = EVulkanAllocationClassCandidate::TransferReadback,
				.PageSize = 128,
				.MaxPageCount = 1,
				.DebugName = "TransferArenaFailure"});
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
			auto Failed = FailingArena.Acquire(64, 16, 1);
			EXPECT_TRUE(Failed.bAllocationFailed);
			EXPECT_FALSE(Failed.Range);
		}

		GDynamicRHI->RHIResetMemoryStatistics();
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		constexpr uint32 PageSize = 8 * 1024 * 1024;
		Durin::FByteArray Bytes(PageSize, std::byte{0x5a});
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
			Durin::FByteArray Actual;
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				Commands, Texture, 0, 0, Actual));
			const std::span<const std::byte> ExpectedBytes =
				std::as_bytes(std::span{Expected});
			EXPECT_EQ(Actual, (Durin::FByteArray(
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
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);

		const FVulkanCompletionTestStats Submitted =
			GetVulkanCompletionTestStats();
		EXPECT_GT(Submitted.LastSubmittedToken, 0u);
		EXPECT_EQ(Submitted.LastReservedToken, Submitted.LastSubmittedToken);
		EXPECT_GT(Submitted.PendingSubmissionCount, 0u);

		Buffer = nullptr;
		Commands.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		const uint64 PendingDeletes =
			GetVulkanMemoryBaselineStatistics().DeferredDeletePendingCount;
		EXPECT_GT(PendingDeletes, 0u);

		WaitForAllVulkanSubmissionsForTesting();
		ReleaseCompletedVulkanResourcesForTesting();
		const FVulkanCompletionTestStats Completed =
			GetVulkanCompletionTestStats();
		EXPECT_EQ(Completed.CompletedToken, Submitted.LastSubmittedToken);
		EXPECT_EQ(Completed.PendingSubmissionCount, 0u);
		EXPECT_LT(GetVulkanMemoryBaselineStatistics().DeferredDeletePendingCount,
			PendingDeletes);
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

#include "../../RDGTestAccess.h"
#include "PCH.VulkanRHI.h"
#include "VulkanResourceState.h"

#include <gtest/gtest.h>

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RDG.h"
#include "InlineRHITestScope.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRHIPrivate.h"
#include "VulkanRHITestEnvironment.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		// Publishes test-owned counted Vulkan resources through the production RDG
		// allocation contract without introducing a second ownership path.
		class FTransitionTestRDGAllocator final : public FRDGAllocator
		{
		public:
			FTransitionTestRDGAllocator(FBufferRHIRef InBuffer,
				FTextureRHIRef InTexture, bool bInFail = false)
				: Buffer(std::move(InBuffer)), Texture(std::move(InTexture)),
				bFail(bInFail)
			{
			}

			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& OutResources, std::string& OutError)
				-> bool override
			{
				if (bFail)
				{
					OutError = "injected allocation failure";
					return false;
				}
				for (const FRDGAllocationRequest& Request : Requests)
				{
					const bool bPublished =
						Request.Kind == ERDGResourceKind::Texture
						? OutResources.SetTexture(Request.ResourceId, Texture,
							Request.ResourceId + 1)
						: OutResources.SetBuffer(Request.ResourceId, Buffer,
							Request.ResourceId + 1);
					if (!bPublished)
					{
						OutError = "test allocator could not publish resource";
						return false;
					}
				}
				OutError.clear();
				return true;
			}

		private:
			FBufferRHIRef Buffer;
			FTextureRHIRef Texture;
			bool bFail = false;
		};
	} // namespace

	TEST(FVulkanQueueOwnershipTests, RequiresExactAcquireAndPreservesUntransferredRanges)
	{
		FVulkanQueueOwnershipTracker State(64);
		ASSERT_TRUE(State.Claim(0, 64, 0));
		ASSERT_TRUE(State.Release(16, 16, 0, 2, 1));
		EXPECT_FALSE(State.CanUse(16, 16, 0));
		EXPECT_FALSE(State.CanUse(16, 16, 2));
		EXPECT_FALSE(State.Acquire(16, 8, 0, 2, 1));
		EXPECT_FALSE(State.Acquire(16, 16, 0, 1, 1));
		EXPECT_FALSE(State.Acquire(16, 16, 1, 2, 1));
		EXPECT_FALSE(State.Acquire(16, 16, 0, 2, 2));
		EXPECT_FALSE(State.Release(16, 16, 0, 2, 2));
		EXPECT_FALSE(State.Claim(16, 16, 0));
		EXPECT_TRUE(State.CanUse(0, 16, 0));
		EXPECT_TRUE(State.CanUse(32, 32, 0));
		ASSERT_TRUE(State.Acquire(16, 16, 0, 2, 1));
		EXPECT_FALSE(State.Acquire(16, 16, 0, 2, 1));
		EXPECT_TRUE(State.CanUse(16, 16, 2));
		EXPECT_FALSE(State.CanUse(0, 64, 0));
		EXPECT_FALSE(State.CanUse(0, 64, 2));
		ASSERT_TRUE(State.Release(16, 16, 2, 0, 3));
		ASSERT_TRUE(State.Acquire(16, 16, 2, 0, 3));
		EXPECT_TRUE(State.CanUse(0, 64, 0));
	}

	TEST(FVulkanQueueOwnershipTests, IndependentTransfersAndInvalidRangesDoNotMutateOwnership)
	{
		FVulkanQueueOwnershipTracker State(64);
		ASSERT_TRUE(State.Release(0, 16, 0, 2, 1));
		ASSERT_TRUE(State.Release(32, 16, 0, 1, 2));
		EXPECT_FALSE(State.Release(16, 16, 0, 1, 1));
		EXPECT_FALSE(State.Release(63, UINT64_MAX, 0, 1, 3));
		EXPECT_FALSE(State.Claim(64, 1, 0));
		EXPECT_FALSE(State.Claim(0, 0, 0));
		EXPECT_FALSE(State.Claim(16, 16, VK_QUEUE_FAMILY_IGNORED));
		ASSERT_TRUE(State.Acquire(32, 16, 0, 1, 2));
		EXPECT_TRUE(State.CanUse(32, 16, 1));
		EXPECT_TRUE(State.IsReleased(0, 16));
		EXPECT_TRUE(State.Claim(16, 16, 2));
		EXPECT_FALSE(State.Claim(16, 16, 0));
	}

	TEST(FVulkanQueueOwnershipTests, ReleasedBufferRejectsEvenDiscardUntilAcquire)
	{
		FVulkanBufferStateTracker State(64);
		State.Apply(0, 64, ERHIAccess::TransferWrite);
		ASSERT_TRUE(State.GetOwnership().Release(16, 16, 0, 2, 1));
		ERHIAccess Tracked = ERHIAccess::None;
		EXPECT_FALSE(State.Validate(16, 16, ERHIAccess::Discard, Tracked));
		EXPECT_TRUE(State.Validate(0, 16, ERHIAccess::TransferWrite, Tracked));
		ASSERT_TRUE(State.GetOwnership().Acquire(16, 16, 0, 2, 1));
		EXPECT_TRUE(State.Validate(16, 16, ERHIAccess::TransferWrite, Tracked));
	}

	TEST(FVulkanQueueOwnershipTests, TextureOwnershipSeparatesAspectsLayersAndMips)
	{
		FVulkanTextureStateTracker State(3, 2);
		const FRHITextureSubresourceRange Depth{ERHITextureAspect::Depth, 1, 1, 0, 1};
		ASSERT_TRUE(State.ClaimOwnership(Depth, 0));
		EXPECT_FALSE(State.ClaimOwnership(Depth, 2));
		EXPECT_FALSE(State.ClaimOwnership({ERHITextureAspect::Depth, 0, 3, 0, 2}, 2));
		EXPECT_TRUE(State.ClaimOwnership({ERHITextureAspect::Depth, 0, 1, 0, 1}, 1));
		EXPECT_TRUE(State.ClaimOwnership({ERHITextureAspect::Depth, 1, 1, 1, 1}, 2));
		EXPECT_TRUE(State.ClaimOwnership({ERHITextureAspect::Stencil, 1, 1, 0, 1}, 2));
		EXPECT_FALSE(State.ClaimOwnership({ERHITextureAspect::Color, 2, UINT32_MAX, 0, 1}, 0));
	}

	TEST(FVulkanQueueOwnershipTests, TextureAcquireMustCoverTheEntireReleasedSubresourceSet)
	{
		FVulkanTextureStateTracker State(3, 2);
		const auto Aspects = ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		const FRHITextureSubresourceRange Range{Aspects, 1, 2, 0, 2};
		State.Apply(Range, ERHIAccess::TransferWrite);
		ASSERT_TRUE(State.ClaimOwnership(Range, 0));
		ASSERT_TRUE(State.ReleaseOwnership(Range, 0, 2, 1));
		ERHIAccess Tracked = ERHIAccess::None;
		EXPECT_FALSE(State.Validate(Range, ERHIAccess::Discard, Tracked));
		EXPECT_FALSE(State.ClaimOwnership(Range, 0));
		EXPECT_FALSE(State.ClaimOwnership(Range, 2));
		EXPECT_FALSE(State.AcquireOwnership({ERHITextureAspect::Depth, 1, 2, 0, 2}, 0, 2, 1));
		EXPECT_FALSE(State.AcquireOwnership({Aspects, 1, 2, 0, 1}, 0, 2, 1));
		EXPECT_FALSE(State.AcquireOwnership({Aspects, 0, 3, 0, 2}, 0, 2, 1));
		EXPECT_TRUE(State.ClaimOwnership({Aspects, 0, 1, 0, 2}, 1));
		ASSERT_TRUE(State.AcquireOwnership(Range, 0, 2, 1));
		EXPECT_TRUE(State.ClaimOwnership(Range, 2));
		EXPECT_TRUE(State.Validate(Range, ERHIAccess::TransferWrite, Tracked));
		EXPECT_FALSE(State.AcquireOwnership(Range, 0, 2, 1));
	}

	TEST(FVulkanQueueOwnershipTests, RejectedMultiRangeReleaseIsTransactional)
	{
		FVulkanQueueOwnershipTracker State(64);
		ASSERT_TRUE(State.Claim(32, 16, 2));
		const std::array Ranges{FVulkanQueueOwnershipTracker::FRange{0, 16},
			FVulkanQueueOwnershipTracker::FRange{32, 16}};
		EXPECT_FALSE(State.ReleaseRanges(Ranges, 0, 1, 3));
		EXPECT_TRUE(State.Claim(0, 16, 1));
		EXPECT_FALSE(State.IsReleased(32, 16));
		const std::array Overlap{FVulkanQueueOwnershipTracker::FRange{16, 16},
			FVulkanQueueOwnershipTracker::FRange{24, 8}};
		EXPECT_FALSE(State.ReleaseRanges(Overlap, 0, 1, 4));
		EXPECT_TRUE(State.Claim(16, 16, 2));
	}

	TEST(FVulkanResourceTransitionMappingTests, SeparatesGraphicsAndComputeShaderIntent)
	{
		const auto Graphics = MapVulkanResourceState(ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Graphics.StageMask2, vk::PipelineStageFlagBits2::eAllGraphics);
		EXPECT_EQ(Graphics.LegacyStageMask, vk::PipelineStageFlagBits::eAllGraphics);

		const auto Compute = MapVulkanResourceState(ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Compute.StageMask2, vk::PipelineStageFlagBits2::eComputeShader);
		EXPECT_EQ(Compute.LegacyStageMask, vk::PipelineStageFlagBits::eComputeShader);
		EXPECT_TRUE(Compute.AccessMask2 & vk::AccessFlagBits2::eShaderRead);
		EXPECT_TRUE(Compute.AccessMask2 & vk::AccessFlagBits2::eShaderWrite);
		EXPECT_TRUE(Compute.LegacyAccessMask & vk::AccessFlagBits::eShaderRead);
		EXPECT_TRUE(Compute.LegacyAccessMask & vk::AccessFlagBits::eShaderWrite);
	}

	TEST(FVulkanResourceTransitionMappingTests, MapsSync2AndLegacyToEquivalentIntent)
	{
		const std::array States{
			ERHIAccess::VertexBufferRead, ERHIAccess::IndexBufferRead,
			ERHIAccess::GraphicsUniformRead, ERHIAccess::ComputeUniformRead,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead,
			ERHIAccess::TransferRead, ERHIAccess::HostRead,
			ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::DepthStencilReadWrite,
			ERHIAccess::GraphicsShaderReadWrite, ERHIAccess::ComputeShaderReadWrite,
			ERHIAccess::TransferWrite, ERHIAccess::HostWrite, ERHIAccess::Present};
		for (ERHIAccess State : States)
		{
			const auto Mapping = MapVulkanResourceState(State);
			if (State != ERHIAccess::Present)
			{
				EXPECT_NE(Mapping.StageMask2, vk::PipelineStageFlags2{});
				EXPECT_NE(Mapping.LegacyStageMask, vk::PipelineStageFlags{});
			}
			EXPECT_EQ(Mapping.AccessMask2 == vk::AccessFlags2{},
				Mapping.LegacyAccessMask == vk::AccessFlags{});
		}
		EXPECT_EQ(MapVulkanResourceState(ERHIAccess::TransferRead).Layout,
			vk::ImageLayout::eTransferSrcOptimal);
		EXPECT_EQ(MapVulkanResourceState(ERHIAccess::TransferWrite).Layout,
			vk::ImageLayout::eTransferDstOptimal);
	}

	TEST(FVulkanResourceTransitionTests, DiscardRetainsAllTrackedSourceScopes)
	{
		FVulkanBufferStateTracker Buffer(64);
		Buffer.Apply(0, 16, ERHIAccess::ComputeShaderRead);
		Buffer.Apply(16, 16, ERHIAccess::TransferWrite);
		Buffer.Apply(32, 32, ERHIAccess::VertexBufferRead);
		const auto BufferSource = Buffer.GetBarrierSource(0, 32);
		const auto Compute = MapVulkanResourceState(ERHIAccess::ComputeShaderRead);
		const auto Transfer = MapVulkanResourceState(ERHIAccess::TransferWrite);
		EXPECT_EQ(BufferSource.StageMask2, Compute.StageMask2 | Transfer.StageMask2);
		EXPECT_EQ(BufferSource.AccessMask2, Compute.AccessMask2 | Transfer.AccessMask2);
		EXPECT_EQ(BufferSource.LegacyStageMask, Compute.LegacyStageMask | Transfer.LegacyStageMask);
		EXPECT_EQ(BufferSource.LegacyAccessMask, Compute.LegacyAccessMask | Transfer.LegacyAccessMask);

		FVulkanTextureStateTracker Texture(3, 1);
		Texture.Apply({ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIAccess::ComputeShaderRead);
		Texture.Apply({ERHITextureAspect::Color, 1, 1, 0, 1}, ERHIAccess::TransferWrite);
		Texture.Apply({ERHITextureAspect::Color, 2, 1, 0, 1}, ERHIAccess::GraphicsShaderRead);
		const auto TextureSource = Texture.GetBarrierSource(
			{ERHITextureAspect::Color, 0, 2, 0, 1}, true);
		EXPECT_EQ(TextureSource.StageMask2, BufferSource.StageMask2);
		EXPECT_EQ(TextureSource.AccessMask2, BufferSource.AccessMask2);
		EXPECT_EQ(TextureSource.LegacyStageMask, BufferSource.LegacyStageMask);
		EXPECT_EQ(TextureSource.LegacyAccessMask, BufferSource.LegacyAccessMask);
		EXPECT_EQ(TextureSource.Layout, vk::ImageLayout::eUndefined);
		EXPECT_EQ(Texture.GetBarrierSource(
			{ERHITextureAspect::Color, 0, 1, 0, 1}, false).Layout,
			vk::ImageLayout::eShaderReadOnlyOptimal);
	}

	TEST(FVulkanResourceTransitionTests, BufferIntervalsSplitAndMergeDeterministically)
	{
		FVulkanBufferStateTracker Tracker(64);
		ERHIAccess Tracked = ERHIAccess::Discard;
		EXPECT_TRUE(Tracker.Validate(8, 16, ERHIAccess::None, Tracked));
		Tracker.Apply(8, 16, ERHIAccess::TransferWrite);
		EXPECT_EQ(Tracker.GetIntervals(), (std::vector<FVulkanBufferStateTracker::FInterval>{
			{0, 8, ERHIAccess::None}, {8, 16, ERHIAccess::TransferWrite}, {24, 40, ERHIAccess::None}}));
		EXPECT_FALSE(Tracker.Validate(0, 32, ERHIAccess::None, Tracked));
		EXPECT_TRUE(Tracker.Validate(0, 32, ERHIAccess::Discard, Tracked));
		Tracker.Apply(0, 32, ERHIAccess::VertexBufferRead);
		Tracker.Apply(32, 32, ERHIAccess::VertexBufferRead);
		EXPECT_EQ(Tracker.GetIntervals(), (std::vector<FVulkanBufferStateTracker::FInterval>{
			{0, 64, ERHIAccess::VertexBufferRead}}));
	}

	TEST(FVulkanResourceTransitionTests, DrawAccessValidationIgnoresUnusedBufferCapacity)
	{
		FVulkanBufferStateTracker Tracker(64);
		Tracker.Apply(0, 48, ERHIAccess::IndexBufferRead);

		ERHIAccess Tracked = ERHIAccess::None;
		EXPECT_TRUE(Tracker.Validate(0, 48, ERHIAccess::IndexBufferRead, Tracked));
		EXPECT_EQ(Tracked, ERHIAccess::IndexBufferRead);
		EXPECT_FALSE(Tracker.Validate(0, 64, ERHIAccess::IndexBufferRead, Tracked));
	}

	TEST(FVulkanResourceTransitionTests, TextureStateDoesNotBleedAcrossPlanesMipsOrLayers)
	{
		FVulkanTextureStateTracker Tracker(3, 2);
		const FRHITextureSubresourceRange Range{ERHITextureAspect::Depth, 1, 1, 1, 1};
		Tracker.Apply(Range, ERHIAccess::DepthStencilReadWrite);
		EXPECT_EQ(Tracker.Get(ERHITextureAspect::Depth, 1, 1), ERHIAccess::DepthStencilReadWrite);
		EXPECT_EQ(Tracker.Get(ERHITextureAspect::Stencil, 1, 1), ERHIAccess::None);
		EXPECT_EQ(Tracker.Get(ERHITextureAspect::Depth, 0, 1), ERHIAccess::None);
		EXPECT_EQ(Tracker.Get(ERHITextureAspect::Depth, 1, 0), ERHIAccess::None);
		ERHIAccess Tracked = ERHIAccess::Discard;
		EXPECT_TRUE(Tracker.Validate(Range, ERHIAccess::DepthStencilReadWrite, Tracked));
		EXPECT_FALSE(Tracker.Validate({ERHITextureAspect::Depth, 0, 2, 1, 1},
			ERHIAccess::None, Tracked));
	}

	TEST(FVulkanResourceTransitionTests,
		DualUseTextureDescriptorLayoutFollowsBindingAndExactTrackedRange)
	{
		FVulkanTextureStateTracker Tracker(2, 2);
		const FRHITextureSubresourceRange Range{
			ERHITextureAspect::Color, 1, 1, 1, 1};
		ERHIAccess Tracked = ERHIAccess::None;

		Tracker.Apply(Range, ERHIAccess::GraphicsShaderReadWrite);
		EXPECT_TRUE(ValidateVulkanTextureDescriptorState(
			Tracker, Range, ERHIBindingType::StorageImage, Tracked));
		EXPECT_FALSE(ValidateVulkanTextureDescriptorState(
			Tracker, Range, ERHIBindingType::Texture, Tracked));
		EXPECT_EQ(GetVulkanDescriptorImageLayout(ERHIBindingType::StorageImage),
			vk::ImageLayout::eGeneral);

		Tracker.Apply(Range, ERHIAccess::GraphicsShaderRead);
		EXPECT_TRUE(ValidateVulkanTextureDescriptorState(
			Tracker, Range, ERHIBindingType::Texture, Tracked));
		EXPECT_FALSE(ValidateVulkanTextureDescriptorState(
			Tracker, {ERHITextureAspect::Color, 0, 2, 1, 1},
			ERHIBindingType::Texture, Tracked));
		EXPECT_EQ(GetVulkanDescriptorImageLayout(ERHIBindingType::Texture),
			vk::ImageLayout::eShaderReadOnlyOptimal);
	}

	TEST(FVulkanResourceTransitionTests, HardwareRecordsBufferAndDisjointTextureTransitions)
	{
		FInlineRHITestScope Scope;

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		struct FBarrierOverrideScope
		{
			~FBarrierOverrideScope()
			{
				SetVulkanBarrierPathOverrideForTest(std::nullopt);
			}
		} BarrierOverrideScope;
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = RHICreateBuffer(FRHIBufferCreateDesc::Create(
			"TransitionHardwareBuffer", 64, 4,
			EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer));
		FBufferRHIRef BurstBuffer = RHICreateBuffer(FRHIBufferCreateDesc::Create(
			"TransitionBurstBuffer", 64, 1,
			EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer));
		FTextureRHIRef Texture = RHICreateTexture(FRHITextureCreateDesc::Create2D(
			"TransitionHardwareTexture", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetNumMips(2).SetFlags(ETextureCreateFlags::ShaderResource));
		ASSERT_TRUE(Buffer && BurstBuffer && Texture);
		ResetVulkanHotPathWorkTestStats();
		SetVulkanBarrierPathOverrideForTest(false);

		const std::array BufferWriteTransitions{FRHIBufferTransition::Whole(
			Buffer.GetReference(), ERHIAccess::Discard, ERHIAccess::TransferWrite)};
		const std::array BufferReadTransitions{FRHIBufferTransition::Whole(
			Buffer.GetReference(), ERHIAccess::TransferWrite, ERHIAccess::VertexBufferRead)};
		const std::array TextureWriteTransitions{
			FRHITextureTransition{Texture.GetReference(), {ERHITextureAspect::Color, 1, 1, 0, 1},
				ERHIAccess::Discard, ERHIAccess::TransferWrite}};
		const std::array TextureReadTransitions{
			FRHITextureTransition{Texture.GetReference(), {ERHITextureAspect::Color, 1, 1, 0, 1},
				ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}};
		Commands.TransitionBuffers(BufferWriteTransitions);
		Commands.TransitionTextures(TextureWriteTransitions);
		Commands.TransitionBuffers(BufferReadTransitions);
		Commands.TransitionTextures(TextureReadTransitions);
		std::vector<FRHIBufferTransition> BurstWriteTransitions;
		std::vector<FRHIBufferTransition> BurstReadTransitions;
		for (uint64 Offset = 0; Offset < 64; ++Offset)
		{
			BurstWriteTransitions.push_back({BurstBuffer.GetReference(), Offset, 1,
				ERHIAccess::Discard, ERHIAccess::TransferWrite});
			BurstReadTransitions.push_back({BurstBuffer.GetReference(), Offset, 1,
				ERHIAccess::TransferWrite, ERHIAccess::VertexBufferRead});
		}
		Commands.TransitionBuffers(BurstWriteTransitions);
		Commands.TransitionBuffers(BurstReadTransitions);
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
		const FVulkanHotPathWorkTestStats LegacyWork =
			GetVulkanHotPathWorkTestStats();
		EXPECT_EQ(LegacyWork.Sync2BufferBarriers, 0u);
		EXPECT_EQ(LegacyWork.LegacyBufferBarriers, 130u);
		EXPECT_EQ(LegacyWork.Sync2ImageBarriers, 0u);
		EXPECT_EQ(LegacyWork.LegacyImageBarriers, 2u);

		if (GDynamicRHI->RHIGetCapabilities()->bSupportsSynchronization2)
		{
			ResetVulkanHotPathWorkTestStats();
			SetVulkanBarrierPathOverrideForTest(true);
			Commands.TransitionBuffers(std::array{FRHIBufferTransition::Whole(
				Buffer.GetReference(), ERHIAccess::VertexBufferRead,
				ERHIAccess::TransferWrite)});
			Commands.TransitionBuffers(std::array{FRHIBufferTransition::Whole(
				Buffer.GetReference(), ERHIAccess::TransferWrite,
				ERHIAccess::VertexBufferRead)});
			Commands.TransitionTextures(std::array{FRHITextureTransition{
				Texture.GetReference(), {ERHITextureAspect::Color, 1, 1, 0, 1},
				ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferWrite}});
			Commands.TransitionTextures(std::array{FRHITextureTransition{
				Texture.GetReference(), {ERHITextureAspect::Color, 1, 1, 0, 1},
				ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
			for (FRHIBufferTransition& Transition : BurstWriteTransitions)
				Transition.ExpectedBefore = ERHIAccess::VertexBufferRead;
			Commands.TransitionBuffers(BurstWriteTransitions);
			Commands.TransitionBuffers(BurstReadTransitions);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			const FVulkanHotPathWorkTestStats Sync2Work =
				GetVulkanHotPathWorkTestStats();
			EXPECT_EQ(Sync2Work.Sync2BufferBarriers, 130u);
			EXPECT_EQ(Sync2Work.LegacyBufferBarriers, 0u);
			EXPECT_EQ(Sync2Work.Sync2ImageBarriers, 2u);
			EXPECT_EQ(Sync2Work.LegacyImageBarriers, 0u);
		}

		auto* VulkanBuffer = static_cast<FVulkanBuffer*>(Buffer.GetReference());
		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture.GetReference());
		EXPECT_EQ(VulkanBuffer->GetStateTracker().GetIntervals(),
			(std::vector<FVulkanBufferStateTracker::FInterval>{{0, 64, ERHIAccess::VertexBufferRead}}));
		EXPECT_EQ(VulkanTexture->GetStateTracker().Get(ERHITextureAspect::Color, 1, 0),
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(VulkanTexture->GetStateTracker().Get(ERHITextureAspect::Color, 0, 0),
			ERHIAccess::None);

		Buffer = nullptr;
		BurstBuffer = nullptr;
		Texture = nullptr;
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		RHIExit();
	}

	TEST(FVulkanResourceTransitionTests, FailedGraphRetainsItsSubmittedPrefixWithoutPublishingExtraction)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto& Commands = FRHICommandListImmediate::Get();
		auto Buffer = RHICreateBuffer(FRHIBufferCreateDesc::Create("PartialGraph", 64, 4,
			EBufferUsageFlags::Static | EBufferUsageFlags::DestinationCopy));
		ASSERT_TRUE(Buffer);
		FBufferRHIRef Extracted;
		FRHIGPUSubmissionTicket Prefix;
		{
			FRDGBuilder Graph;
			const auto Resource = Graph.RegisterExternalBuffer(Buffer, "PartialGraph",
				ERHIAccess::None, ERHIAccess::TransferWrite);
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Graph, "SubmitThenFail", ERDGPassType::Copy,
				[&](FRHICommandListImmediate& List, const FRDGPassResources&) {
					List.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
					Prefix = GetLastVulkanSubmissionTicketForTesting();
					throw std::runtime_error("injected graph callback failure");
				});
			FRDGBuilderTestAccessor::UseBuffer(Graph, Pass, Resource, 0, 64,
				ERDGUse::Write, ERHIAccess::TransferWrite, true);
			Graph.QueueBufferExtraction(Resource, &Extracted, ERHIAccess::TransferWrite);
			EXPECT_THROW(Graph.Execute(Commands), std::runtime_error);
			EXPECT_FALSE(Extracted);
			EXPECT_EQ(Prefix.GetState(), ERHIGPUSubmissionState::Submitted);
		}
		// The builder no longer owns the resource, but the submitted command storage does.
		EXPECT_GT(Buffer->GetRefCount(), 1u);
		EXPECT_FALSE(Prefix.IsRetirementEligible());
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
		EXPECT_EQ(GDynamicRHI->RHIWaitForCompletion(Prefix, 1'000'000'000), ERHIGPUWaitResult::Complete);
		WaitForAllVulkanSubmissionsForTesting();
	}

	TEST(FVulkanResourceTransitionTests, RenderGraphTransitionsReplayThroughVulkanStateTracking)
	{
		FInlineRHITestScope Scope;
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = RHICreateBuffer(FRHIBufferCreateDesc::Create(
			"GraphBuffer", 64, 4, EBufferUsageFlags::Static
				| EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::DestinationCopy));
		const auto TextureDesc = FRHITextureCreateDesc::Create2D(
			"GraphTexture", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetNumMips(2).SetFlags(ETextureCreateFlags::DestinationCopy
				| ETextureCreateFlags::ShaderResource);
		FTextureRHIRef Texture = RHICreateTexture(TextureDesc);
		ASSERT_TRUE(Buffer && Texture);
		bool bExecuted = false;
		{
			FRDGBuilder RejectedBuilder;
			RejectedBuilder.EnablePassCulling();
			const auto RejectedBuffer = RejectedBuilder.CreateBuffer(
				FRDGBufferDesc{.Buffer = Buffer->GetDesc()}, "RejectedBuffer");
			const auto RejectedPass = FRDGBuilderTestAccessor::AddPass(RejectedBuilder, "Rejected",
				ERDGPassType::Copy,
				[&](FRHICommandListImmediate&, const FRDGPassResources&) {
					bExecuted = true;
				});
			FRDGBuilderTestAccessor::UseBuffer(RejectedBuilder, RejectedPass, RejectedBuffer, 0, 64,
				ERDGUse::Write, ERHIAccess::TransferWrite, true);
			RejectedBuilder.MarkPassRoot(RejectedPass, "external-effect");
			std::string AllocationError;
			{
				FTransitionTestRDGAllocator RejectedAllocator(Buffer, Texture, true);
				FRDGExecutionContext RejectedContext{RejectedAllocator};
				const auto Rejected = RejectedBuilder.Execute(Commands, &RejectedContext);
				EXPECT_EQ(Rejected.Status, ERDGExecutionStatus::PreparationFailed);
				AllocationError = Rejected.Result.Message;
			}
			EXPECT_FALSE(bExecuted);
			EXPECT_TRUE(RejectedBuilder.GetSubmissionReceipts().empty());
			EXPECT_EQ(AllocationError, "injected allocation failure");

			FRDGBuilder Builder;
			const auto GraphBuffer = Builder.CreateBuffer(
				FRDGBufferDesc{.Buffer = Buffer->GetDesc()}, "GraphBuffer",
				ERHIAccess::VertexBufferRead);
			const auto GraphTexture = Builder.CreateTexture(
				FRDGTextureDesc{.Texture = TextureDesc}, "GraphTexture",
				ERHIAccess::GraphicsShaderRead);
			const auto Copy = FRDGBuilderTestAccessor::AddPass(Builder, "Copy", ERDGPassType::Copy);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Copy, GraphBuffer, 0, 64, ERDGUse::Write,
				ERHIAccess::TransferWrite, true);
			FRDGBuilderTestAccessor::UseTexture(Builder, Copy, GraphTexture,
				{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Write,
				ERHIAccess::TransferWrite, true);
			const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder,
				"Consume", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Consume, GraphBuffer, 0, 64, ERDGUse::Read,
				ERHIAccess::VertexBufferRead);
			FRDGBuilderTestAccessor::UseTexture(Builder, Consume, GraphTexture,
				{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read,
				ERHIAccess::GraphicsShaderRead);
			{
				FTransitionTestRDGAllocator Allocator(Buffer, Texture);
				FRDGExecutionContext Context{Allocator};
				const auto Result = Builder.Execute(Commands, &Context);
				ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
				EXPECT_EQ(Builder.Execute(Commands, &Context).Status, ERDGExecutionStatus::InvalidState);
			}
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);

			auto* VulkanBuffer = static_cast<FVulkanBuffer*>(Buffer.GetReference());
			ASSERT_EQ(Builder.GetSubmissionReceipts().size(), Builder.GetExecutionPlan().Batches.size());
			for (const auto& Receipt : Builder.GetSubmissionReceipts())
			{
				EXPECT_EQ(Receipt.GetState(), ERHIGPUSubmissionState::Submitted);
				EXPECT_EQ(Receipt.GetTicket().GetPoint(), Builder.GetSubmissionReceipts().back().GetTicket().GetPoint());
			}
			auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture.GetReference());
			EXPECT_EQ(VulkanBuffer->GetStateTracker().GetIntervals(),
				(std::vector<FVulkanBufferStateTracker::FInterval>{
					{0, 64, ERHIAccess::VertexBufferRead}}));
			EXPECT_EQ(VulkanTexture->GetStateTracker().Get(
				ERHITextureAspect::Color, 1, 0), ERHIAccess::GraphicsShaderRead);
			EXPECT_EQ(VulkanTexture->GetStateTracker().Get(
				ERHITextureAspect::Color, 0, 0), ERHIAccess::None);

			FRDGBuilder Compact;
			const auto CompactTexture = Compact.RegisterExternalTexture(Texture, "CompactTexture",
				ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead);
			const auto CompactWrite = FRDGBuilderTestAccessor::AddPass(Compact, "CompactWrite", ERDGPassType::Copy);
			FRDGBuilderTestAccessor::UseTexture(Compact, CompactWrite, CompactTexture,
				{ERHITextureAspect::Color, 0, 2, 0, 1}, ERDGUse::Write, ERHIAccess::TransferWrite, true);
			const auto CompactResult = Compact.Execute(Commands);
			ASSERT_TRUE(CompactResult.IsSuccess()) << CompactResult.Result.Message;
			EXPECT_EQ(Compact.GetStatistics().TextureTransitions, 2u);
			EXPECT_EQ(Compact.GetStatistics().TextureTransitionSubresources, 4u);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
			for (uint32 Mip = 0; Mip < 2; ++Mip)
				EXPECT_EQ(VulkanTexture->GetStateTracker().Get(
					ERHITextureAspect::Color, Mip, 0), ERHIAccess::GraphicsShaderRead);

			FRDGBuilder Next;
			const auto External = Next.RegisterExternalBuffer(Buffer, "ExternalHandoff",
				ERHIAccess::VertexBufferRead, ERHIAccess::TransferWrite);
			const auto Rewrite = FRDGBuilderTestAccessor::AddPass(Next, "Rewrite", ERDGPassType::Copy);
			FRDGBuilderTestAccessor::UseBuffer(Next, Rewrite, External, 0, 64, ERDGUse::Write,
				ERHIAccess::TransferWrite, true);
			const auto Handoff = Next.Execute(Commands);
			ASSERT_TRUE(Handoff.IsSuccess()) << Handoff.Result.Message;
			ASSERT_EQ(Next.GetPasses()[0].Barriers.GetBufferTransitions().size(), 1u);
			EXPECT_EQ(Next.GetPasses()[0].Barriers.GetBufferTransitions()[0].ExpectedBefore,
				ERHIAccess::VertexBufferRead);
			const auto CommandCount = Commands.GetNumRecordedCommands();
			EXPECT_EQ(Next.Execute(Commands).Status, ERDGExecutionStatus::InvalidState);
			EXPECT_EQ(Commands.GetNumRecordedCommands(), CommandCount);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			EXPECT_EQ(VulkanBuffer->GetStateTracker().GetIntervals(),
				(std::vector<FVulkanBufferStateTracker::FInterval>{
					{0, 64, ERHIAccess::TransferWrite}}));
		}
		Buffer = nullptr;
		Texture = nullptr;
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		RHIExit();
	}
} // namespace Durin::VulkanRHI

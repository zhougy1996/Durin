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
		FRDGBuilder RejectedBuilder;
		RejectedBuilder.EnablePassCulling();
		const auto RejectedBuffer = RejectedBuilder.CreateBuffer(
			FRDGBufferDesc{.Buffer = Buffer->GetDesc()}, "RejectedBuffer");
		const auto RejectedPass = RejectedBuilder.AddPass("Rejected",
			ERDGPassType::Copy,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		RejectedBuilder.UseBuffer(RejectedPass, RejectedBuffer, 0, 64,
			ERDGUse::Write, ERHIAccess::TransferWrite, true);
		RejectedBuilder.MarkPassRoot(RejectedPass, "external-effect");
		auto Rejected = RejectedBuilder.Compile();
		ASSERT_TRUE(Rejected.IsSuccess()) << Rejected.Error;
		std::string AllocationError;
		{
			FTransitionTestRDGAllocator RejectedAllocator(Buffer, Texture, true);
			FRDGExecutionContext RejectedContext{RejectedAllocator};
			EXPECT_FALSE(Rejected.Graph->Execute(
				Commands, RejectedContext, &AllocationError));
		}
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(AllocationError, "injected allocation failure");

		FRDGBuilder Builder;
		const auto GraphBuffer = Builder.CreateBuffer(
			FRDGBufferDesc{.Buffer = Buffer->GetDesc()}, "GraphBuffer",
			ERHIAccess::VertexBufferRead);
		const auto GraphTexture = Builder.CreateTexture(
			FRDGTextureDesc{.Texture = TextureDesc}, "GraphTexture",
			ERHIAccess::GraphicsShaderRead);
		const auto Copy = Builder.AddPass("Copy", ERDGPassType::Copy);
		Builder.UseBuffer(Copy, GraphBuffer, 0, 64, ERDGUse::Write,
			ERHIAccess::TransferWrite, true);
		Builder.UseTexture(Copy, GraphTexture,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Write,
			ERHIAccess::TransferWrite, true);
		const auto Consume = Builder.AddPass(
			"Consume", ERDGPassType::Graphics);
		Builder.UseBuffer(Consume, GraphBuffer, 0, 64, ERDGUse::Read,
			ERHIAccess::VertexBufferRead);
		Builder.UseTexture(Consume, GraphTexture,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Compiled = Builder.Compile();
		ASSERT_TRUE(Compiled.IsSuccess()) << Compiled.Error;
		{
			FTransitionTestRDGAllocator Allocator(Buffer, Texture);
			FRDGExecutionContext Context{Allocator};
			std::string Error;
			ASSERT_TRUE(Compiled.Graph->Execute(Commands, Context, &Error)) << Error;
		}
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);

		auto* VulkanBuffer = static_cast<FVulkanBuffer*>(Buffer.GetReference());
		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture.GetReference());
		EXPECT_EQ(VulkanBuffer->GetStateTracker().GetIntervals(),
			(std::vector<FVulkanBufferStateTracker::FInterval>{
				{0, 64, ERHIAccess::VertexBufferRead}}));
		EXPECT_EQ(VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, 1, 0), ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, 0, 0), ERHIAccess::None);

		Compiled.Graph.reset();
		Buffer = nullptr;
		Texture = nullptr;
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		RHIExit();
	}
} // namespace Durin::VulkanRHI

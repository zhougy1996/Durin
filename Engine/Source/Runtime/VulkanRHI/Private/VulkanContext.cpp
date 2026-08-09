#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanPendingState.h"
#include "VulkanRenderPass.h"
#include "VulkanViewport.h"
#include "VulkanQueue.h"
#include "VulkanSubmission.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRHIPrivate.h"
#include "VulkanResourceState.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto ValidateAttachment(const FRHIAttachmentLayout& Layout, const FRHITexture* Texture, uint32 Width, uint32 Height, const char* Label) -> void
		{
			checkf(Texture != nullptr, "{} attachment texture is null.", Label);
			checkf(Texture->GetFormat() == Layout.Format, "{} attachment format does not match the render target layout.", Label);
			checkf(Texture->GetNumSamples() == Layout.NumSamples, "{} attachment sample count does not match the render target layout.", Label);
			checkf(Texture->GetSizeX() == Width && Texture->GetSizeY() == Height, "{} attachment extent does not match the render pass extent.", Label);
			checkf(Layout.LoadAction != ERHIRenderTargetLoadAction::Load || Layout.InitialLayout != ERHITextureLayout::Undefined,
				"{} attachment cannot load from an undefined initial layout.", Label);
		}

		auto ValidateRenderPassInfo(const FRHIRenderPassInfo& Info) -> void
		{
			const auto& Layout = Info.RenderTargetLayout;
			checkf(Layout.IsValid(), "Render target layout is invalid.");
			checkf(Layout.NumColorRenderTargets <= MaxSimultaneousRenderTargets, "Render pass color attachment count exceeds the RHI limit.");
			checkf(Layout.NumColorRenderTargets > 0 || Layout.bHasDepthStencil, "Render pass must contain at least one attachment.");
			FRHITexture* ExtentSource = Layout.NumColorRenderTargets > 0 ? Info.ColorRenderTargets[0] : Info.DepthStencilRenderTarget;
			check(ExtentSource != nullptr);
			const uint32 Width = ExtentSource->GetSizeX();
			const uint32 Height = ExtentSource->GetSizeY();
			uint8 RasterSamples = 0;
			for (uint32 Index = 0; Index < Layout.NumColorRenderTargets; ++Index)
			{
				const auto& Color = Layout.ColorAttachments[Index];
				ValidateAttachment(Color.RenderTarget, Info.ColorRenderTargets[Index], Width, Height, "Color");
				RasterSamples = RasterSamples == 0 ? Color.RenderTarget.NumSamples : RasterSamples;
				checkf(RasterSamples == Color.RenderTarget.NumSamples, "All primary render attachments must use the same sample count.");
				if (Color.bHasResolveTarget)
				{
					ValidateAttachment(Color.ResolveTarget, Info.ColorResolveTargets[Index], Width, Height, "Resolve");
					checkf(Color.RenderTarget.NumSamples > 1, "Resolve attachments require a multisampled color attachment.");
					checkf(Color.ResolveTarget.NumSamples == 1, "Resolve attachments must be single-sampled.");
					checkf(Color.ResolveTarget.Format == Color.RenderTarget.Format, "Resolve and color attachment formats must match.");
				}
				else
				{
					checkf(Info.ColorResolveTargets[Index] == nullptr, "A resolve texture was bound without a resolve attachment layout.");
				}
			}
			if (Layout.bHasDepthStencil)
			{
				ValidateAttachment(Layout.DepthStencilAttachment, Info.DepthStencilRenderTarget, Width, Height, "Depth/stencil");
				RasterSamples = RasterSamples == 0 ? Layout.DepthStencilAttachment.NumSamples : RasterSamples;
				checkf(RasterSamples == Layout.DepthStencilAttachment.NumSamples, "Depth and color attachments must use the same sample count.");
			}
			else
			{
				checkf(Info.DepthStencilRenderTarget == nullptr, "A depth/stencil texture was bound without a depth/stencil layout.");
			}
		}
	}
	FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue)
		: RHI(InRHI)
		, Device(InDevice)
		, Queue(InQueue)
		, PendingGfxState(std::make_unique<FVulkanPendingGraphicsState>(InDevice))
	{
		CheckVulkanRHIThread();
		Pool = new FVulkanCommandBufferPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
	}

	FVulkanCommandListContext::~FVulkanCommandListContext()
	{
		CheckVulkanRHIThread();
		PendingGfxState.reset();
		delete Pool;
	}

	auto FVulkanCommandListContext::RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->SetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FVulkanCommandListContext::RHISetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->SetScissor(MinX, MinY, Width, Height);
	}

	auto FVulkanCommandListContext::RHIBeginFrame(
		const FRHIBeginFrameArgs&) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->ClearDescriptorSetCache();
	}

	auto FVulkanCommandListContext::RHISubmitCommands() -> void
	{
		CheckVulkanRHIThread();
		if (!Payloads.empty())
		{
			Finalize();
		}
	}

	auto FVulkanCommandListContext::RHIEndFrame() -> void
	{
		CheckVulkanRHIThread();
		FVulkanFrame& Frame = Device.GetCurrentFrame();
		FVulkanPayload& Payload = GetPayload();
		Payload.Fence = Frame.GetFrameFence();
		Finalize();

		Pool->FreeUnusedCommandBuffers(Queue);
	}

	auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName DebugName) -> void
	{
		CheckVulkanRHIThread();
		ValidateRenderPassInfo(RenderPassInfo);
		check(PendingAttachmentStates.empty());
		std::vector<FPendingAttachmentState> CandidateStates;
		auto ValidateAndQueue = [&CandidateStates](FRHITexture* TextureRHI,
			const FRHIAttachmentLayout& Attachment) {
			auto* Texture = static_cast<FVulkanTexture*>(TextureRHI);
			const FRHITextureSubresourceRange Range{
				GetTextureAspects(Texture->GetFormat()), 0, 1, 0, 1};
			const ERHIAccess Expected = Attachment.InitialLayout == ERHITextureLayout::Undefined
				? ERHIAccess::Discard : Attachment.InitialAccess;
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(Texture->GetStateTracker().Validate(Range, Expected, Tracked),
				"Vulkan render-pass attachment state mismatch: resource={}, expected={}, tracked={}, requested={}.",
				static_cast<const void*>(Texture), static_cast<uint32>(Expected),
				static_cast<uint32>(Tracked), static_cast<uint32>(Attachment.FinalAccess));
			CandidateStates.push_back({Texture, Range, Attachment.FinalAccess});
		};
		for (uint32 Index = 0; Index < RenderPassInfo.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			const FRHIColorAttachmentLayout& Attachment =
				RenderPassInfo.RenderTargetLayout.ColorAttachments[Index];
			ValidateAndQueue(RenderPassInfo.ColorRenderTargets[Index], Attachment.RenderTarget);
			if (Attachment.bHasResolveTarget)
			{
				ValidateAndQueue(RenderPassInfo.ColorResolveTargets[Index], Attachment.ResolveTarget);
			}
		}
		if (RenderPassInfo.RenderTargetLayout.bHasDepthStencil)
		{
			ValidateAndQueue(RenderPassInfo.DepthStencilRenderTarget,
				RenderPassInfo.RenderTargetLayout.DepthStencilAttachment);
		}
		// Pass names are deliberately excluded from render-pass identity and only annotate GPU work.
		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		FVulkanRenderPass* RenderPass = RenderPassManager.GetOrCreateRenderPass(RenderPassInfo.RenderTargetLayout);
		FVulkanFramebuffer* Framebuffer = RenderPassManager.GetOrCreateFrameBuffer(RenderPassInfo, *RenderPass);
		RenderPassManager.BeginRenderPass(*this, Device, GetCommandBuffer(), RenderPassInfo, RenderPass, Framebuffer, DebugName);
		PendingAttachmentStates = std::move(CandidateStates);
	}

	auto FVulkanCommandListContext::RHIEndRenderPass() -> void
	{
		CheckVulkanRHIThread();
		Device.GetRenderPassManager().EndRenderPass(GetCommandBuffer());
		for (const FPendingAttachmentState& State : PendingAttachmentStates)
		{
			State.Texture->GetStateTracker().Apply(State.Range, State.FinalAccess);
		}
		PendingAttachmentStates.clear();
	}

	auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
	{
		CheckVulkanRHIThread();
		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);
		VulkanViewport->BeginDrawing();
	}

	auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		CheckVulkanRHIThread();
		if (!bPresent)
		{
			return;
		}

		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);

		FVulkanQueue* PresentQueue = Device.GetPresentQueue();
		VulkanViewport->Present(*this, *GetCommandBuffer(), *PresentQueue, bLockToVsync);
	}

	auto FVulkanCommandListContext::RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->SetGraphicsPipelineState(static_cast<FVulkanGraphicsPipelineState&>(GraphicsPipelineState), GetCommandBuffer()->GetHandle());
	}

	auto FVulkanCommandListContext::RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void
	{
		CheckVulkanRHIThread();
		if (InVertexBuffer != nullptr)
		{
			vk::Buffer BufferHandle = static_cast<FVulkanBuffer*>(InVertexBuffer)->GetHandle();
			GetCommandBuffer()->GetHandle().bindVertexBuffers(StreamIndex, BufferHandle, {Offset});
		}
	}

	static constexpr auto DeduceIndexType(uint32 Stride) -> vk::IndexType
	{
		if (Stride == 2) { return vk::IndexType::eUint16; }
		if (Stride == 4) { return vk::IndexType::eUint32; }
		checkf(false, "Unsupported index buffer stride: {}", Stride);
		return vk::IndexType::eUint16;
	}

	auto FVulkanCommandListContext::RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void
	{
		CheckVulkanRHIThread();
		if (InIndexBuffer != nullptr)
		{
			const FVulkanBuffer* IndexBuffer = static_cast<FVulkanBuffer*>(InIndexBuffer);
			GetCommandBuffer()->GetHandle().bindIndexBuffer(IndexBuffer->GetHandle(), Offset, DeduceIndexType(IndexBuffer->GetStride()));
		}
	}

	auto FVulkanCommandListContext::RHITransitionBuffers(
		std::span<const FRHIBufferTransition> Transitions) -> void
	{
		CheckVulkanRHIThread();
		std::vector<vk::BufferMemoryBarrier2> Barriers2;
		std::vector<vk::BufferMemoryBarrier> LegacyBarriers;
		vk::PipelineStageFlags LegacySourceStages;
		vk::PipelineStageFlags LegacyDestinationStages;
		Barriers2.reserve(Transitions.size());
		LegacyBarriers.reserve(Transitions.size());
		for (const FRHIBufferTransition& Transition : Transitions)
		{
			auto* Buffer = static_cast<FVulkanBuffer*>(Transition.Buffer);
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(Buffer->GetStateTracker().Validate(
				Transition.Offset, Transition.Size, Transition.ExpectedBefore, Tracked),
				"Vulkan buffer transition state mismatch: resource={}, offset={}, size={}, expected={}, tracked={}, requested={}.",
				static_cast<const void*>(Buffer), Transition.Offset, Transition.Size,
				static_cast<uint32>(Transition.ExpectedBefore), static_cast<uint32>(Tracked),
				static_cast<uint32>(Transition.RequiredAfter));
			const ERHIAccess SourceAccess = Transition.ExpectedBefore == ERHIAccess::Discard
				? ERHIAccess::None : Tracked;
			const FVulkanResourceStateMapping Source = MapVulkanResourceState(SourceAccess);
			const FVulkanResourceStateMapping Destination = MapVulkanResourceState(Transition.RequiredAfter);
			Barriers2.push_back(vk::BufferMemoryBarrier2()
				.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
				.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
				.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setBuffer(Buffer->GetHandle()).setOffset(Transition.Offset).setSize(Transition.Size));
			LegacyBarriers.push_back(vk::BufferMemoryBarrier()
				.setSrcAccessMask(Source.LegacyAccessMask).setDstAccessMask(Destination.LegacyAccessMask)
				.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setBuffer(Buffer->GetHandle()).setOffset(Transition.Offset).setSize(Transition.Size));
			LegacySourceStages |= Source.LegacyStageMask;
			LegacyDestinationStages |= Destination.LegacyStageMask;
		}
		vk::CommandBuffer CommandBuffer = GetCommandBuffer()->GetHandle();
		const FRHICapabilities* Capabilities = RHI->RHIGetCapabilities();
		check(Capabilities);
		if (Capabilities->bSupportsSynchronization2)
		{
			CommandBuffer.pipelineBarrier2(vk::DependencyInfo().setBufferMemoryBarriers(Barriers2));
		}
		else
		{
			CommandBuffer.pipelineBarrier(LegacySourceStages, LegacyDestinationStages,
				vk::DependencyFlags{}, {}, LegacyBarriers, {});
		}
		for (const FRHIBufferTransition& Transition : Transitions)
		{
			static_cast<FVulkanBuffer*>(Transition.Buffer)->GetStateTracker().Apply(
				Transition.Offset, Transition.Size, Transition.RequiredAfter);
		}
	}

	auto FVulkanCommandListContext::RHITransitionTextures(
		std::span<const FRHITextureTransition> Transitions) -> void
	{
		CheckVulkanRHIThread();
		std::vector<vk::ImageMemoryBarrier2> Barriers2;
		std::vector<vk::ImageMemoryBarrier> LegacyBarriers;
		vk::PipelineStageFlags LegacySourceStages;
		vk::PipelineStageFlags LegacyDestinationStages;
		Barriers2.reserve(Transitions.size());
		LegacyBarriers.reserve(Transitions.size());
		for (const FRHITextureTransition& Transition : Transitions)
		{
			auto* Texture = static_cast<FVulkanTexture*>(Transition.Texture);
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(Texture->GetStateTracker().Validate(Transition.Range, Transition.ExpectedBefore, Tracked),
				"Vulkan texture transition state mismatch: resource={}, aspects={}, mip={}+{}, layer={}+{}, expected={}, tracked={}, requested={}.",
				static_cast<const void*>(Texture), static_cast<uint32>(Transition.Range.Aspects),
				Transition.Range.FirstMip, Transition.Range.NumMips,
				Transition.Range.FirstArrayLayer, Transition.Range.NumArrayLayers,
				static_cast<uint32>(Transition.ExpectedBefore), static_cast<uint32>(Tracked),
				static_cast<uint32>(Transition.RequiredAfter));
			const ERHIAccess SourceAccess = Transition.ExpectedBefore == ERHIAccess::Discard
				? ERHIAccess::None : Tracked;
			const FVulkanResourceStateMapping Source = MapVulkanResourceState(SourceAccess);
			const FVulkanResourceStateMapping Destination = MapVulkanResourceState(Transition.RequiredAfter);
			const vk::ImageSubresourceRange Range(ToVulkanAspectFlags(Transition.Range.Aspects),
				Transition.Range.FirstMip, Transition.Range.NumMips,
				Transition.Range.FirstArrayLayer, Transition.Range.NumArrayLayers);
			Barriers2.push_back(vk::ImageMemoryBarrier2()
				.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
				.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
				.setOldLayout(Source.Layout).setNewLayout(Destination.Layout)
				.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setImage(Texture->Image).setSubresourceRange(Range));
			LegacyBarriers.push_back(vk::ImageMemoryBarrier()
				.setSrcAccessMask(Source.LegacyAccessMask).setDstAccessMask(Destination.LegacyAccessMask)
				.setOldLayout(Source.Layout).setNewLayout(Destination.Layout)
				.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setImage(Texture->Image).setSubresourceRange(Range));
			LegacySourceStages |= Source.LegacyStageMask;
			LegacyDestinationStages |= Destination.LegacyStageMask;
		}
		vk::CommandBuffer CommandBuffer = GetCommandBuffer()->GetHandle();
		const FRHICapabilities* Capabilities = RHI->RHIGetCapabilities();
		check(Capabilities);
		if (Capabilities->bSupportsSynchronization2)
		{
			CommandBuffer.pipelineBarrier2(vk::DependencyInfo().setImageMemoryBarriers(Barriers2));
		}
		else
		{
			CommandBuffer.pipelineBarrier(LegacySourceStages, LegacyDestinationStages,
				vk::DependencyFlags{}, {}, {}, LegacyBarriers);
		}
		for (const FRHITextureTransition& Transition : Transitions)
		{
			static_cast<FVulkanTexture*>(Transition.Texture)->GetStateTracker().Apply(
				Transition.Range, Transition.RequiredAfter);
		}
	}

	auto FVulkanCommandListContext::RHIWriteBuffer(
		FRHIBuffer* Buffer,
		uint32 Offset,
		std::span<const uint8> Data) -> void
	{
		CheckVulkanRHIThread();
		check(Buffer);
		static_cast<FVulkanBuffer*>(Buffer)->Write(*this, Offset, Data);
	}

	auto FVulkanCommandListContext::RHIInitializeTexture(
		FRHITexture* Texture) -> void
	{
		CheckVulkanRHIThread();
		RHI->InitializeTexture(*this, Texture);
	}

	auto FVulkanCommandListContext::RHIUpdateTexture2D(
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::span<const uint8> SourceData) -> void
	{
		CheckVulkanRHIThread();
		RHI->UpdateTexture2D(
			*this, Texture, MipIndex, ArraySlice,
			UpdateRegion, SourcePitch, SourceData);
	}

	auto FVulkanCommandListContext::RHIReadTexture2D(
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		std::vector<uint8>& OutData) -> bool
	{
		CheckVulkanRHIThread();
		return RHI->ReadTexture2D(
			*this, Texture, MipIndex, ArraySlice, OutData);
	}

	auto FVulkanCommandListContext::RHIAllocateDynamicUniformBuffer(
		const void* Data,
		uint32 Size) -> FRHIUniformBufferRange
	{
		CheckVulkanRHIThread();
		FRHIUniformBufferRange Result;
		const uint32 FrameIndex = Device.GetCurrentFrameIndex();
		requiref(Device.GetDynamicUniformBufferAllocator().TryAllocate(FrameIndex, Data, Size, Result),
			"Direct context allocation requires a prepared dynamic-uniform page.");
		auto* Buffer = static_cast<FVulkanBuffer*>(Result.Buffer);
		Buffer->GetStateTracker().Apply(Result.Offset, Result.Size, ERHIAccess::HostWrite);
		const std::array Transition{FRHIBufferTransition{
			Buffer, Result.Offset, Result.Size, ERHIAccess::HostWrite, ERHIAccess::GraphicsUniformRead}};
		RHITransitionBuffers(Transition);
		return Result;
	}

	auto FVulkanCommandListContext::RHIAcquireBackBuffer(
		FRHITexture* BackBuffer) -> void
	{
		CheckVulkanRHIThread();
		check(BackBuffer);
		static_cast<FVulkanBackBuffer*>(BackBuffer)->AcquireBackBufferImage(*this);
	}

	auto FVulkanCommandListContext::RHIBlockUntilGPUIdle() -> void
	{
		CheckVulkanRHIThread();
		Finalize();
		Device.WaitUtilIdle();
	}

	auto FVulkanCommandListContext::RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		CheckVulkanRHIThread();
		FVulkanGraphicsPipelineState* PipelineState = PendingGfxState->GetPipelineState();
		check(PipelineState);
		PipelineState->PushConstants(*this, StageFlags, Offset, Size, Data);
	}

	auto FVulkanCommandListContext::RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanCommandListContext::RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->PrepareForDraw(*this);
		GetCommandBuffer()->GetHandle().drawIndexed(IndexCount, 1, StartIndexLocation, VertexOffset, 0);
	}

	auto FVulkanCommandListContext::GetCommandBuffer() -> FVulkanCommandBuffer*
	{
		CheckVulkanRHIThread();
		FVulkanPayload& Payload = GetPayload();
		if (Payload.CommandBuffers.empty())
		{
			PrepareNewCommandBuffer(Payload);
		}
		return Payload.CommandBuffers.back();
	}

	auto FVulkanCommandListContext::RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void
	{
		CheckVulkanRHIThread();
	}

	auto FVulkanCommandListContext::AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void
	{
		CheckVulkanRHIThread();
		AddWaitSemaphores(InWaitFlag, std::span(&InWaitSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void
	{
		CheckVulkanRHIThread();
		auto& Payload = GetPayload();
		Payload.WaitSemaphores.insert(Payload.WaitSemaphores.end(), InWaitSemaphores.begin(), InWaitSemaphores.end());
		Payload.WaitFlags.insert(Payload.WaitFlags.end(), InWaitSemaphores.size(), InWaitFlag);
	}

	auto FVulkanCommandListContext::AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void
	{
		CheckVulkanRHIThread();
 		AddSignalSemaphores(std::span(&InSignalSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void
	{
		CheckVulkanRHIThread();
		auto& Payload = GetPayload();
		Payload.SignalSemaphores.insert(Payload.SignalSemaphores.end(), InSignalSemaphores.begin(), InSignalSemaphores.end());
	}

	auto FVulkanCommandListContext::NotifyDeleted_Image(vk::Image Image) -> void
	{
		CheckVulkanRHIThread();
	}

	auto FVulkanCommandListContext::NotifyDeleted_GraphicsPipeline(
		FVulkanGraphicsPipelineState* PipelineState) -> void
	{
		CheckVulkanRHIThread();
		PendingGfxState->NotifyDeletedPipeline(PipelineState);
	}

	auto FVulkanCommandListContext::Finalize() -> void
	{
		CheckVulkanRHIThread();
		GetCommandBuffer()->End();
		Queue->SubmitPayloads(Payloads);
		Payloads.clear();
	}

	auto FVulkanCommandListContext::PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void
	{
		CheckVulkanRHIThread();
		check(InPayload.CommandBuffers.empty());
		FVulkanCommandBuffer* NewCmdBuffer = Pool->Create();
		InPayload.CommandBuffers.push_back(NewCmdBuffer);
		NewCmdBuffer->Begin();
	}

	auto FVulkanCommandListContext::GetPayload() -> FVulkanPayload&
	{
		CheckVulkanRHIThread();
		// Currently only support one payload per submit.
		if (Payloads.empty())
		{
			Payloads.push_back(new FVulkanPayload(*Queue));
		}
		return *Payloads.back();
	}

	auto FVulkanDynamicRHI::RHIGetDefaultContext() -> IRHICommandContext*
	{
		CheckVulkanRHIThread();
		return Device->GetImmediateContext();
	}

	auto FVulkanDynamicRHI::RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*
	{
		CheckVulkanRHIThread();
		if (Pipeline != ERHIPipeline::Graphics)
		{
			return nullptr;
		}

		return Device->GetImmediateContext();
	}
} // namespace Durin::VulkanRHI

#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanGPUTiming.h"
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
#include "VulkanView.h"

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
		, PendingComputeState(std::make_unique<FVulkanPendingComputeState>(InDevice))
	{
		CheckVulkanRHIThread();
		Pool = new FVulkanCommandBufferPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
	}

	FVulkanCommandListContext::~FVulkanCommandListContext()
	{
		CheckVulkanRHIThread();
		PendingGfxState.reset();
		PendingComputeState.reset();
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
		PendingComputeState->ClearDescriptorSetCache();
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
		Frame.SetLastSubmittedToken(Finalize());

		Pool->FreeUnusedCommandBuffers(Queue);
	}

	auto FVulkanCommandListContext::RHIBeginDiagnosticRegion(
		std::string_view Name) -> void
	{
		CheckVulkanRHIThread();
		GetCommandBuffer()->BeginDiagnosticRegion(Name);
	}

	auto FVulkanCommandListContext::RHIEndDiagnosticRegion() -> void
	{
		CheckVulkanRHIThread();
		GetCommandBuffer()->EndDiagnosticRegion();
	}

	auto FVulkanCommandListContext::RHIBeginGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		Device.GetGPUTimingManager().Begin(*GetCommandBuffer(),
			*static_cast<FVulkanGPUTimingQuery*>(Query));
	}

	auto FVulkanCommandListContext::RHIEndGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		auto* VulkanQuery = static_cast<FVulkanGPUTimingQuery*>(Query);
		Device.GetGPUTimingManager().End(*GetCommandBuffer(), *VulkanQuery);
		PendingTimingQueries.push_back(VulkanQuery);
	}

	auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& InRenderPassInfo, FName DebugName) -> void
	{
		CheckVulkanRHIThread();
		FRHIRenderPassInfo CanonicalInfo = InRenderPassInfo;
		std::array<FTextureViewRHIRef, MaxSimultaneousRenderTargets> ColorViews;
		std::array<FTextureViewRHIRef, MaxSimultaneousRenderTargets> ResolveViews;
		FTextureViewRHIRef DepthView;
		for (uint32 Index = 0; Index < CanonicalInfo.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			if (!CanonicalInfo.ColorRenderTargetViews[Index] && CanonicalInfo.ColorRenderTargets[Index])
			{
				ColorViews[Index] = RHI->RHICreateTextureView(
					CanonicalInfo.ColorRenderTargets[Index], MakeDefaultTextureViewDesc(
						*CanonicalInfo.ColorRenderTargets[Index], ERHITextureViewUsage::ColorAttachment));
				CanonicalInfo.ColorRenderTargetViews[Index] = ColorViews[Index].GetReference();
			}
			if (CanonicalInfo.RenderTargetLayout.ColorAttachments[Index].bHasResolveTarget
				&& !CanonicalInfo.ColorResolveTargetViews[Index] && CanonicalInfo.ColorResolveTargets[Index])
			{
				ResolveViews[Index] = RHI->RHICreateTextureView(
					CanonicalInfo.ColorResolveTargets[Index], MakeDefaultTextureViewDesc(
						*CanonicalInfo.ColorResolveTargets[Index], ERHITextureViewUsage::ColorAttachment));
				CanonicalInfo.ColorResolveTargetViews[Index] = ResolveViews[Index].GetReference();
			}
		}
		if (CanonicalInfo.RenderTargetLayout.bHasDepthStencil
			&& !CanonicalInfo.DepthStencilRenderTargetView && CanonicalInfo.DepthStencilRenderTarget)
		{
			DepthView = RHI->RHICreateTextureView(
				CanonicalInfo.DepthStencilRenderTarget, MakeDefaultTextureViewDesc(
					*CanonicalInfo.DepthStencilRenderTarget, ERHITextureViewUsage::DepthStencilAttachment));
			CanonicalInfo.DepthStencilRenderTargetView = DepthView.GetReference();
		}
		const FRHIRenderPassInfo& RenderPassInfo = CanonicalInfo;
		ValidateRenderPassInfo(RenderPassInfo);
		check(PendingAttachmentStates.empty());
		std::vector<FPendingAttachmentState> CandidateStates;
		auto ValidateAndQueue = [&CandidateStates](FRHITextureView* TextureViewRHI,
			const FRHIAttachmentLayout& Attachment) {
			check(TextureViewRHI);
			auto* TextureView = static_cast<FVulkanTextureView*>(TextureViewRHI);
			auto* Texture = static_cast<FVulkanTexture*>(TextureView->GetTexture());
			const FRHITextureSubresourceRange Range = TextureView->GetDesc().Range;
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
			ValidateAndQueue(RenderPassInfo.ColorRenderTargetViews[Index], Attachment.RenderTarget);
			if (Attachment.bHasResolveTarget)
			{
				ValidateAndQueue(RenderPassInfo.ColorResolveTargetViews[Index], Attachment.ResolveTarget);
			}
		}
		if (RenderPassInfo.RenderTargetLayout.bHasDepthStencil)
		{
			ValidateAndQueue(RenderPassInfo.DepthStencilRenderTargetView,
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

	auto FVulkanCommandListContext::RHICopyBuffer(FRHIBuffer* SourceRHI, FRHIBuffer* DestinationRHI,
		std::span<const FRHIBufferCopyRegion> Regions) -> void
	{
		CheckVulkanRHIThread();
		std::string Error;
		checkf(ValidateBufferCopies(SourceRHI, DestinationRHI, Regions, Error),
			"Invalid Vulkan buffer copy replay: {}", Error);
		auto* Source = static_cast<FVulkanBuffer*>(SourceRHI);
		auto* Destination = static_cast<FVulkanBuffer*>(DestinationRHI);
		std::vector<vk::BufferCopy> NativeRegions;
		NativeRegions.reserve(Regions.size());
		for (const auto& Region : Regions)
		{
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(Source->GetStateTracker().Validate(Region.SourceOffset, Region.Size,
				ERHIAccess::TransferRead, Tracked), "Vulkan buffer copy source is not in TransferRead.");
			checkf(Destination->GetStateTracker().Validate(Region.DestinationOffset, Region.Size,
				ERHIAccess::TransferWrite, Tracked), "Vulkan buffer copy destination is not in TransferWrite.");
			NativeRegions.emplace_back(Region.SourceOffset, Region.DestinationOffset, Region.Size);
		}
		auto* CommandBuffer = GetCommandBuffer();
		CommandBuffer->BeginDiagnosticRegion("Durin.Transfer.BufferCopy");
		CommandBuffer->GetHandle().copyBuffer(
			Source->GetHandle(), Destination->GetHandle(), NativeRegions);
		CommandBuffer->EndDiagnosticRegion();
	}

	auto FVulkanCommandListContext::RHICopyBufferToTexture(FRHIBuffer* SourceRHI, FRHITexture* DestinationRHI,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		CheckVulkanRHIThread();
		std::string Error;
		checkf(ValidateBufferToTextureCopies(SourceRHI, DestinationRHI, Regions, Error),
			"Invalid Vulkan buffer-to-texture replay: {}", Error);
		auto* Source = static_cast<FVulkanBuffer*>(SourceRHI);
		auto* Destination = static_cast<FVulkanTexture*>(DestinationRHI);
		std::vector<vk::BufferImageCopy> NativeRegions;
		NativeRegions.reserve(Regions.size());
		for (const auto& Region : Regions)
		{
			uint64 Footprint = 0;
			check(GetBufferTextureCopyFootprint(*Destination, Region, Footprint, Error));
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(Source->GetStateTracker().Validate(Region.BufferOffset, Footprint,
				ERHIAccess::TransferRead, Tracked), "Vulkan buffer-to-texture source is not in TransferRead.");
			const FRHITextureSubresourceRange Range{Region.TextureAspect, Region.TextureMip, 1,
				Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers};
			checkf(Destination->GetStateTracker().Validate(Range, ERHIAccess::TransferWrite, Tracked),
				"Vulkan buffer-to-texture destination is not in TransferWrite.");
			NativeRegions.emplace_back(
				Region.BufferOffset, Region.BufferRowLength, Region.BufferImageHeight,
				vk::ImageSubresourceLayers(ToVulkanAspectFlags(Region.TextureAspect),
					Region.TextureMip, Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers),
				vk::Offset3D(Region.TextureOffset.X, Region.TextureOffset.Y, Region.TextureOffset.Z),
				vk::Extent3D(Region.TextureExtent.Width, Region.TextureExtent.Height, Region.TextureExtent.Depth));
		}
		auto* CommandBuffer = GetCommandBuffer();
		CommandBuffer->BeginDiagnosticRegion("Durin.Transfer.BufferToTexture");
		CommandBuffer->GetHandle().copyBufferToImage(Source->GetHandle(), Destination->Image,
			vk::ImageLayout::eTransferDstOptimal, NativeRegions);
		CommandBuffer->EndDiagnosticRegion();
	}

	auto FVulkanCommandListContext::RHICopyTextureToBuffer(FRHITexture* SourceRHI, FRHIBuffer* DestinationRHI,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		CheckVulkanRHIThread();
		std::string Error;
		checkf(ValidateTextureToBufferCopies(SourceRHI, DestinationRHI, Regions, Error),
			"Invalid Vulkan texture-to-buffer replay: {}", Error);
		auto* Source = static_cast<FVulkanTexture*>(SourceRHI);
		auto* Destination = static_cast<FVulkanBuffer*>(DestinationRHI);
		std::vector<vk::BufferImageCopy> NativeRegions;
		NativeRegions.reserve(Regions.size());
		for (const auto& Region : Regions)
		{
			uint64 Footprint = 0;
			check(GetBufferTextureCopyFootprint(*Source, Region, Footprint, Error));
			ERHIAccess Tracked = ERHIAccess::None;
			const FRHITextureSubresourceRange Range{Region.TextureAspect, Region.TextureMip, 1,
				Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers};
			checkf(Source->GetStateTracker().Validate(Range, ERHIAccess::TransferRead, Tracked),
				"Vulkan texture-to-buffer source is not in TransferRead.");
			checkf(Destination->GetStateTracker().Validate(Region.BufferOffset, Footprint,
				ERHIAccess::TransferWrite, Tracked), "Vulkan texture-to-buffer destination is not in TransferWrite.");
			NativeRegions.emplace_back(
				Region.BufferOffset, Region.BufferRowLength, Region.BufferImageHeight,
				vk::ImageSubresourceLayers(ToVulkanAspectFlags(Region.TextureAspect),
					Region.TextureMip, Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers),
				vk::Offset3D(Region.TextureOffset.X, Region.TextureOffset.Y, Region.TextureOffset.Z),
				vk::Extent3D(Region.TextureExtent.Width, Region.TextureExtent.Height, Region.TextureExtent.Depth));
		}
		auto* CommandBuffer = GetCommandBuffer();
		CommandBuffer->BeginDiagnosticRegion("Durin.Readback.TextureToBuffer");
		CommandBuffer->GetHandle().copyImageToBuffer(Source->Image,
			vk::ImageLayout::eTransferSrcOptimal, Destination->GetHandle(), NativeRegions);
		CommandBuffer->EndDiagnosticRegion();
	}

	auto FVulkanCommandListContext::RHICopyTexture(FRHITexture* SourceRHI, FRHITexture* DestinationRHI,
		std::span<const FRHITextureCopyRegion> Regions) -> void
	{
		CheckVulkanRHIThread();
		std::string Error;
		checkf(ValidateTextureCopies(SourceRHI, DestinationRHI, Regions, Error),
			"Invalid Vulkan texture copy replay: {}", Error);
		auto* Source = static_cast<FVulkanTexture*>(SourceRHI);
		auto* Destination = static_cast<FVulkanTexture*>(DestinationRHI);
		std::vector<vk::ImageCopy> NativeRegions;
		NativeRegions.reserve(Regions.size());
		for (const auto& Region : Regions)
		{
			ERHIAccess Tracked = ERHIAccess::None;
			const FRHITextureSubresourceRange SourceRange{Region.SourceAspect, Region.SourceMip, 1,
				Region.SourceFirstArrayLayer, Region.NumArrayLayers};
			const FRHITextureSubresourceRange DestinationRange{Region.DestinationAspect,
				Region.DestinationMip, 1, Region.DestinationFirstArrayLayer, Region.NumArrayLayers};
			checkf(Source->GetStateTracker().Validate(SourceRange, ERHIAccess::TransferRead, Tracked),
				"Vulkan texture copy source is not in TransferRead.");
			checkf(Destination->GetStateTracker().Validate(DestinationRange, ERHIAccess::TransferWrite, Tracked),
				"Vulkan texture copy destination is not in TransferWrite.");
			NativeRegions.emplace_back(
				vk::ImageSubresourceLayers(ToVulkanAspectFlags(Region.SourceAspect),
					Region.SourceMip, Region.SourceFirstArrayLayer, Region.NumArrayLayers),
				vk::Offset3D(Region.SourceOffset.X, Region.SourceOffset.Y, Region.SourceOffset.Z),
				vk::ImageSubresourceLayers(ToVulkanAspectFlags(Region.DestinationAspect),
					Region.DestinationMip, Region.DestinationFirstArrayLayer, Region.NumArrayLayers),
				vk::Offset3D(Region.DestinationOffset.X, Region.DestinationOffset.Y, Region.DestinationOffset.Z),
				vk::Extent3D(Region.Extent.Width, Region.Extent.Height, Region.Extent.Depth));
		}
		auto* CommandBuffer = GetCommandBuffer();
		CommandBuffer->BeginDiagnosticRegion("Durin.Transfer.TextureCopy");
		CommandBuffer->GetHandle().copyImage(Source->Image, vk::ImageLayout::eTransferSrcOptimal,
			Destination->Image, vk::ImageLayout::eTransferDstOptimal, NativeRegions);
		CommandBuffer->EndDiagnosticRegion();
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

	auto FVulkanCommandListContext::RHISetComputePipelineState(
		FRHIComputePipelineState& ComputePipelineState) -> void
	{
		CheckVulkanRHIThread();
		PendingComputeState->SetComputePipelineState(
			static_cast<FVulkanComputePipelineState&>(ComputePipelineState),
			GetCommandBuffer()->GetHandle());
	}

	auto FVulkanCommandListContext::RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void
	{
		CheckVulkanRHIThread();
		if (!InVertexBuffer)
		{
			BoundVertexBuffers.erase(StreamIndex);
			return;
		}
		checkf(EnumHasAnyFlags(InVertexBuffer->GetUsage(), EBufferUsageFlags::VertexBuffer),
			"Bound vertex buffer requires VertexBuffer usage.");
		checkf(Offset <= InVertexBuffer->GetSize(),
			"Bound vertex buffer offset exceeds the resource.");
		BoundVertexBuffers[StreamIndex] = {InVertexBuffer, Offset};
		vk::Buffer BufferHandle = static_cast<FVulkanBuffer*>(InVertexBuffer)->GetHandle();
		GetCommandBuffer()->GetHandle().bindVertexBuffers(StreamIndex, BufferHandle, {Offset});
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
		if (!InIndexBuffer)
		{
			BoundIndexBuffer = nullptr;
			BoundIndexBufferOffset = 0;
			return;
		}
		checkf(EnumHasAnyFlags(InIndexBuffer->GetUsage(), EBufferUsageFlags::IndexBuffer),
			"Bound index buffer requires IndexBuffer usage.");
		checkf((InIndexBuffer->GetStride() == 2 || InIndexBuffer->GetStride() == 4)
			&& Offset <= InIndexBuffer->GetSize()
			&& (Offset % InIndexBuffer->GetStride()) == 0,
			"Bound index buffer requires an aligned uint16 or uint32 format.");
		BoundIndexBuffer = InIndexBuffer;
		BoundIndexBufferOffset = Offset;
		const FVulkanBuffer* IndexBuffer = static_cast<FVulkanBuffer*>(InIndexBuffer);
		GetCommandBuffer()->GetHandle().bindIndexBuffer(IndexBuffer->GetHandle(), Offset,
			DeduceIndexType(IndexBuffer->GetStride()));
	}

	auto FVulkanCommandListContext::RHITransitionBuffers(
		std::span<const FRHIBufferTransition> Transitions) -> void
	{
		CheckVulkanRHIThread();
		vk::CommandBuffer CommandBuffer = GetCommandBuffer()->GetHandle();
		const FRHICapabilities* Capabilities = RHI->RHIGetCapabilities();
		check(Capabilities);
		const bool bUseSynchronization2 =
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			SelectVulkanSynchronization2BarrierPath(
				Capabilities->bSupportsSynchronization2);
#else
			Capabilities->bSupportsSynchronization2;
#endif
		if (bUseSynchronization2)
		{
			std::vector<vk::BufferMemoryBarrier2> Barriers;
			Barriers.reserve(Transitions.size());
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
				Barriers.push_back(vk::BufferMemoryBarrier2()
					.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
					.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setBuffer(Buffer->GetHandle()).setOffset(Transition.Offset).setSize(Transition.Size));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GVulkanSync2BufferBarrierCount.fetch_add(1, std::memory_order_relaxed);
#endif
			}
			CommandBuffer.pipelineBarrier2(vk::DependencyInfo().setBufferMemoryBarriers(Barriers));
		}
		else
		{
			std::vector<vk::BufferMemoryBarrier> Barriers;
			vk::PipelineStageFlags SourceStages;
			vk::PipelineStageFlags DestinationStages;
			Barriers.reserve(Transitions.size());
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
				Barriers.push_back(vk::BufferMemoryBarrier()
					.setSrcAccessMask(Source.LegacyAccessMask).setDstAccessMask(Destination.LegacyAccessMask)
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setBuffer(Buffer->GetHandle()).setOffset(Transition.Offset).setSize(Transition.Size));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GVulkanLegacyBufferBarrierCount.fetch_add(1, std::memory_order_relaxed);
#endif
				SourceStages |= Source.LegacyStageMask;
				DestinationStages |= Destination.LegacyStageMask;
			}
			CommandBuffer.pipelineBarrier(SourceStages, DestinationStages,
				vk::DependencyFlags{}, {}, Barriers, {});
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
		vk::CommandBuffer CommandBuffer = GetCommandBuffer()->GetHandle();
		const FRHICapabilities* Capabilities = RHI->RHIGetCapabilities();
		check(Capabilities);
		const bool bUseSynchronization2 =
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			SelectVulkanSynchronization2BarrierPath(
				Capabilities->bSupportsSynchronization2);
#else
			Capabilities->bSupportsSynchronization2;
#endif
		if (bUseSynchronization2)
		{
			std::vector<vk::ImageMemoryBarrier2> Barriers;
			Barriers.reserve(Transitions.size());
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
				Barriers.push_back(vk::ImageMemoryBarrier2()
					.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
					.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
					.setOldLayout(Source.Layout).setNewLayout(Destination.Layout)
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setImage(Texture->Image).setSubresourceRange(Range));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GVulkanSync2ImageBarrierCount.fetch_add(1, std::memory_order_relaxed);
#endif
			}
			CommandBuffer.pipelineBarrier2(vk::DependencyInfo().setImageMemoryBarriers(Barriers));
		}
		else
		{
			std::vector<vk::ImageMemoryBarrier> Barriers;
			vk::PipelineStageFlags SourceStages;
			vk::PipelineStageFlags DestinationStages;
			Barriers.reserve(Transitions.size());
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
				Barriers.push_back(vk::ImageMemoryBarrier()
					.setSrcAccessMask(Source.LegacyAccessMask).setDstAccessMask(Destination.LegacyAccessMask)
					.setOldLayout(Source.Layout).setNewLayout(Destination.Layout)
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setImage(Texture->Image).setSubresourceRange(Range));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
				GVulkanLegacyImageBarrierCount.fetch_add(1, std::memory_order_relaxed);
#endif
				SourceStages |= Source.LegacyStageMask;
				DestinationStages |= Destination.LegacyStageMask;
			}
			CommandBuffer.pipelineBarrier(SourceStages, DestinationStages,
				vk::DependencyFlags{}, {}, {}, Barriers);
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
		requiref(Device.GetDynamicUniformBufferAllocator().TryAllocate(Data, Size, Result),
			"Direct context allocation requires a prepared dynamic-uniform page.");
		auto* Buffer = static_cast<FVulkanBuffer*>(Result.Buffer);
		Buffer->GetStateTracker().Apply(Result.Offset, Result.Size, ERHIAccess::HostWrite);
		const std::array Transition{FRHIBufferTransition{
			Buffer, Result.Offset, Result.Size, ERHIAccess::HostWrite, ERHIAccess::GraphicsUniformRead}};
		RHITransitionBuffers(Transition);
		return Result;
	}

	auto FVulkanCommandListContext::RHIAllocateDynamicStorageBuffer(
		const void* Data, uint32 Size) -> FRHIStorageBufferRange
	{
		CheckVulkanRHIThread();
		FRHIStorageBufferRange Result;
		const uint32 FrameIndex = Device.GetCurrentFrameIndex();
		if (!Device.GetDynamicStorageBufferAllocator().TryAllocate(
			FrameIndex, Data, Size, Result)) return {};
		auto* Buffer = static_cast<FVulkanBuffer*>(Result.Buffer);
		Buffer->GetStateTracker().Apply(Result.Offset, Result.Size, ERHIAccess::HostWrite);
		const std::array Transition{FRHIBufferTransition{
			Buffer, Result.Offset, Result.Size,
			ERHIAccess::HostWrite, ERHIAccess::GraphicsShaderRead}};
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
		if (StageFlags == EShaderStageFlags::Compute)
		{
			PendingComputeState->PushConstants(*this, StageFlags, Offset, Size, Data);
			return;
		}
		FVulkanGraphicsPipelineState* PipelineState = PendingGfxState->GetPipelineState();
		check(PipelineState);
		PipelineState->PushConstants(*this, StageFlags, Offset, Size, Data);
	}

	auto FVulkanCommandListContext::RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		CheckVulkanRHIThread();
		if (InShader && InShader->GetFrequency() == EShaderFrequency::Compute)
			PendingComputeState->SetShaderParameters(InShader, InResourceParameters);
		else PendingGfxState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanCommandListContext::RHIDispatch(uint32 GroupCountX,
		uint32 GroupCountY, uint32 GroupCountZ) -> void
	{
		CheckVulkanRHIThread();
		check(PendingComputeState->GetPipelineState());
		const FRHICapabilities* Capabilities = RHI->RHIGetCapabilities();
		check(Capabilities);
		checkf(GroupCountX > 0 && GroupCountY > 0 && GroupCountZ > 0
			&& GroupCountX <= Capabilities->MaxComputeWorkGroupCount[0]
			&& GroupCountY <= Capabilities->MaxComputeWorkGroupCount[1]
			&& GroupCountZ <= Capabilities->MaxComputeWorkGroupCount[2],
			"Compute dispatch group counts are outside the published limits.");
		PendingComputeState->Dispatch(*this, GroupCountX, GroupCountY, GroupCountZ);
	}

	auto FVulkanCommandListContext::ValidateDrawBindings(uint32 VertexCount,
		uint32 InstanceCount, uint32 FirstVertex, uint32 FirstInstance,
		bool bIndexed) const -> void
	{
		const FVulkanGraphicsPipelineState* PipelineState =
			PendingGfxState->GetPipelineState();
		checkf(PipelineState, "Draw requires an active graphics pipeline.");
		std::unordered_set<uint32> ValidatedStreams;
		for (const FRHIVertexElementIdentity& Element :
			PipelineState->GetKey().VertexElements)
		{
			if (!ValidatedStreams.insert(Element.StreamIndex).second) continue;
			const auto It = BoundVertexBuffers.find(Element.StreamIndex);
			checkf(It != BoundVertexBuffers.end() && It->second.Buffer,
				"Draw is missing required vertex stream {}.", Element.StreamIndex);
			FRHIBuffer* Buffer = It->second.Buffer;
			checkf(EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::VertexBuffer)
				&& (Buffer->GetStride() == 0 || Buffer->GetStride() == Element.Stride)
				&& (It->second.Offset % Element.Stride) == 0,
				"Draw vertex stream {} is incompatible with the declaration.",
				Element.StreamIndex);
			uint64 ElementCount = 1;
			uint64 FirstElement = 0;
			if (Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance)
			{
				ElementCount = InstanceCount;
				FirstElement = FirstInstance;
			}
			else if (!bIndexed)
			{
				ElementCount = VertexCount;
				FirstElement = FirstVertex;
			}
			const uint64 RequiredElements = FirstElement + ElementCount;
			checkf(RequiredElements <= (Buffer->GetSize() - It->second.Offset)
				/ Element.Stride,
				"Draw vertex stream {} range exceeds the bound buffer.",
				Element.StreamIndex);
			const auto* VulkanBuffer = static_cast<const FVulkanBuffer*>(Buffer);
			const uint64 ValidationOffset = It->second.Offset
				+ FirstElement * Element.Stride;
			const uint64 ValidationSize = ElementCount * Element.Stride;
			ERHIAccess Tracked = ERHIAccess::None;
			checkf(VulkanBuffer->GetStateTracker().Validate(ValidationOffset,
				ValidationSize,
				ERHIAccess::VertexBufferRead, Tracked),
				"Draw vertex stream {} range [{}, {}) is not in VertexBufferRead access.",
				Element.StreamIndex, ValidationOffset,
				ValidationOffset + ValidationSize);
		}
	}

	auto FVulkanCommandListContext::RHIDraw(const FRHIDrawArguments& Arguments) -> void
	{
		CheckVulkanRHIThread();
		if (Arguments.VertexCount == 0 || Arguments.InstanceCount == 0) return;
		ValidateDrawBindings(Arguments.VertexCount, Arguments.InstanceCount,
			Arguments.FirstVertex, Arguments.FirstInstance, false);
		PendingGfxState->PrepareForDraw(*this);
		GetCommandBuffer()->GetHandle().draw(Arguments.VertexCount,
			Arguments.InstanceCount, Arguments.FirstVertex, Arguments.FirstInstance);
	}

	auto FVulkanCommandListContext::RHIDrawIndexed(
		const FRHIDrawIndexedArguments& Arguments) -> void
	{
		CheckVulkanRHIThread();
		if (Arguments.IndexCount == 0 || Arguments.InstanceCount == 0) return;
		checkf(BoundIndexBuffer, "Indexed draw requires a bound index buffer.");
		const uint64 AvailableIndices =
			(BoundIndexBuffer->GetSize() - BoundIndexBufferOffset)
			/ BoundIndexBuffer->GetStride();
		checkf(static_cast<uint64>(Arguments.FirstIndex) + Arguments.IndexCount
			<= AvailableIndices, "Indexed draw range exceeds the bound index buffer.");
		const auto* VulkanIndexBuffer =
			static_cast<const FVulkanBuffer*>(BoundIndexBuffer);
		const uint64 IndexValidationOffset = BoundIndexBufferOffset
			+ static_cast<uint64>(Arguments.FirstIndex)
				* BoundIndexBuffer->GetStride();
		const uint64 IndexValidationSize =
			static_cast<uint64>(Arguments.IndexCount)
				* BoundIndexBuffer->GetStride();
		ERHIAccess Tracked = ERHIAccess::None;
		checkf(VulkanIndexBuffer->GetStateTracker().Validate(
			IndexValidationOffset,
			IndexValidationSize,
			ERHIAccess::IndexBufferRead, Tracked),
			"Indexed draw range [{}, {}) is not in IndexBufferRead access.",
			IndexValidationOffset, IndexValidationOffset + IndexValidationSize);
		ValidateDrawBindings(Arguments.IndexCount, Arguments.InstanceCount, 0,
			Arguments.FirstInstance, true);
		PendingGfxState->PrepareForDraw(*this);
		GetCommandBuffer()->GetHandle().drawIndexed(Arguments.IndexCount,
			Arguments.InstanceCount, Arguments.FirstIndex, Arguments.VertexOffset,
			Arguments.FirstInstance);
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

	auto FVulkanCommandListContext::NotifyDeleted_ComputePipeline(
		FVulkanComputePipelineState* PipelineState) -> void
	{
		CheckVulkanRHIThread();
		PendingComputeState->NotifyDeletedPipeline(PipelineState);
	}

	auto FVulkanCommandListContext::Finalize() -> FVulkanCompletionToken
	{
		CheckVulkanRHIThread();
		GetCommandBuffer()->End();
		const FVulkanCompletionToken Token = Queue->SubmitPayloads(Payloads);
		Device.GetGPUTimingManager().MarkSubmitted(Token, PendingTimingQueries);
		PendingTimingQueries.clear();
		Payloads.clear();
		return Token;
	}

	auto FVulkanCommandListContext::AcquireTransferRange(
		EVulkanAllocationClassCandidate AllocationClass, uint64 Size,
		uint64 Alignment) -> FVulkanTransferRange
	{
		CheckVulkanRHIThread();
		check(AllocationClass == EVulkanAllocationClassCandidate::TransferUpload
			|| AllocationClass
				== EVulkanAllocationClassCandidate::TransferReadback);
		FVulkanTransferArena& Arena = AllocationClass
			== EVulkanAllocationClassCandidate::TransferUpload
			? Device.GetUploadArena() : Device.GetReadbackArena();
		for (;;)
		{
			const FVulkanCompletionToken Token = GetPayload().Token;
			FVulkanTransferAcquireResult Result =
				Arena.Acquire(Size, Alignment, Token);
			if (Result.Range)
			{
				return std::move(Result.Range);
			}
			if (Result.bAllocationFailed)
			{
				throw std::runtime_error(std::format(
					"Vulkan transfer arena allocation failed: class={}, bytes={}.",
					static_cast<uint32>(AllocationClass), Size));
			}
			requiref(Result.WaitToken != 0,
				"Vulkan transfer arena exhausted without a retireable range: class={}, bytes={}.",
				static_cast<uint32>(AllocationClass), Size);
			if (Result.WaitToken
				> Device.GetCompletionTracker().GetLastSubmittedToken())
			{
				const FVulkanCompletionToken Submitted = Finalize();
				requiref(Submitted >= Result.WaitToken,
					"Transfer arena range token {} was not submitted by flush token {}.",
					Result.WaitToken, Submitted);
			}
			GVulkanMemoryBaselineTracker.RecordArenaWait(AllocationClass);
			Device.GetCompletionTracker().WaitForToken(Result.WaitToken);
		}
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
			Payloads.push_back(new FVulkanPayload(
				*Queue, Device.GetCompletionTracker().ReserveToken()));
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

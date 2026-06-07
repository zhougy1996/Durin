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

namespace Durin::VulkanRHI
{
	FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue)
		: RHI(InRHI)
		, Device(InDevice)
		, Queue(InQueue)
		, PendingGfxState(std::make_unique<FVulkanPendingGraphicsState>(InDevice))
	{
		Pool = new FVulkanCommandBufferPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
	}

	FVulkanCommandListContext::~FVulkanCommandListContext()
	{
		PendingGfxState.reset();
		delete Pool;
	}

	auto FVulkanCommandListContext::RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		PendingGfxState->SetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FVulkanCommandListContext::RHISetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		PendingGfxState->SetScissor(MinX, MinY, Width, Height);
	}

	auto FVulkanCommandListContext::RHIBeginFrame() -> void
	{
		PendingGfxState->ClearDescriptorSetCache();
	}

	auto FVulkanCommandListContext::RHIEndFrame() -> void
	{
		FVulkanFrame& Frame = Device.GetCurrentFrame();
		FVulkanPayload& Payload = GetPayload();
		Payload.Fence = Frame.GetFrameFence();
		Finalize();

		Pool->FreeUnusedCommandBuffers(Queue);
	}

	auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void
	{
		const auto* VulkanRT = static_cast<FVulkanTexture*>(RenderPassInfo.ColorRenderTargets[0]);

		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		FVulkanRenderPass* RenderPass = Device.GetRenderPassManager().GetOrCreateRenderPass(Name, VulkanRT->Format);

		FRHIRenderTargetsInfo RTInfo{};
		RTInfo.NumColorRenderTargets = 1;
		RTInfo.ColorRenderTargets[0] = RenderPassInfo.ColorRenderTargets[0];
		FVulkanFramebuffer* Framebuffer = RenderPassManager.GetOrCreateFrameBuffer(RTInfo);
		RenderPassManager.BeginRenderPass(*this, Device, GetCommandBuffer(), RenderPassInfo, RenderPass, Framebuffer);
	}

	auto FVulkanCommandListContext::RHIEndRenderPass() -> void
	{
		Device.GetRenderPassManager().EndRenderPass(GetCommandBuffer());
	}

	auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
	{
		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);
		VulkanViewport->BeginDrawing(FRHICommandListImmediate::Get());
	}

	auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
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
		PendingGfxState->SetGraphicsPipelineState(static_cast<FVulkanGraphicsPipelineState&>(GraphicsPipelineState), GetCommandBuffer()->GetHandle());
	}

	auto FVulkanCommandListContext::RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void
	{
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
		if (InIndexBuffer != nullptr)
		{
			const FVulkanBuffer* IndexBuffer = static_cast<FVulkanBuffer*>(InIndexBuffer);
			GetCommandBuffer()->GetHandle().bindIndexBuffer(IndexBuffer->GetHandle(), Offset, DeduceIndexType(IndexBuffer->GetStride()));
		}
	}

	auto FVulkanCommandListContext::RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		FVulkanGraphicsPipelineState* PipelineState = PendingGfxState->GetPipelineState();
		check(PipelineState);
		PipelineState->PushConstants(*this, StageFlags, Offset, Size, Data);
	}

	auto FVulkanCommandListContext::RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		PendingGfxState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanCommandListContext::RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		PendingGfxState->PrepareForDraw(*this);
		GetCommandBuffer()->GetHandle().drawIndexed(IndexCount, 1, StartIndexLocation, VertexOffset, 0);
	}

	auto FVulkanCommandListContext::GetCommandBuffer() -> FVulkanCommandBuffer*
	{
		FVulkanPayload& Payload = GetPayload();
		if (Payload.CommandBuffers.empty())
		{
			PrepareNewCommandBuffer(Payload);
		}
		return Payload.CommandBuffers.back();
	}

	auto FVulkanCommandListContext::RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void
	{
	}

	auto FVulkanCommandListContext::AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void
	{
		AddWaitSemaphores(InWaitFlag, std::span(&InWaitSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.WaitSemaphores.insert(Payload.WaitSemaphores.end(), InWaitSemaphores.begin(), InWaitSemaphores.end());
		Payload.WaitFlags.insert(Payload.WaitFlags.end(), InWaitSemaphores.size(), InWaitFlag);
	}

	auto FVulkanCommandListContext::AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void
	{
 		AddSignalSemaphores(std::span(&InSignalSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.SignalSemaphores.insert(Payload.SignalSemaphores.end(), InSignalSemaphores.begin(), InSignalSemaphores.end());
	}

	auto FVulkanCommandListContext::NotifyDeleted_Image(vk::Image Image) -> void
	{
	}

	auto FVulkanCommandListContext::Finalize() -> void
	{
		GetCommandBuffer()->End();
		Queue->SubmitPayloads(Payloads);
		Payloads.clear();
	}

	auto FVulkanCommandListContext::PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void
	{
		check(InPayload.CommandBuffers.empty());
		FVulkanCommandBuffer* NewCmdBuffer = Pool->Create();
		InPayload.CommandBuffers.push_back(NewCmdBuffer);
		NewCmdBuffer->Begin();
	}

	auto FVulkanCommandListContext::GetPayload() -> FVulkanPayload&
	{
		// Currently only support one payload per submit.
		if (Payloads.empty())
		{
			Payloads.push_back(new FVulkanPayload(*Queue));
		}
		return *Payloads.back();
	}

	auto FVulkanDynamicRHI::RHIGetDefaultContext() -> IRHICommandContext*
	{
		return Device->GetImmediateContext();
	}

	auto FVulkanDynamicRHI::RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*
	{
		if (Pipeline != ERHIPipeline::Graphics)
		{
			return nullptr;
		}

		return Device->GetImmediateContext();
	}
} // namespace Durin::VulkanRHI

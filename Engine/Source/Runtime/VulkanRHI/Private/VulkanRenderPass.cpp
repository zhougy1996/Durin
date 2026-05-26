#include "VulkanRenderPass.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanFramebuffer.h"
#include "VulkanTexture.h"

namespace Durin::VulkanRHI
{
	FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& InDevice, vk::Format InFormat)
		: Device(InDevice)
	{
		// Color buffer attachement
		vk::AttachmentDescription ColorAttachment;
		ColorAttachment
			.setSamples(vk::SampleCountFlagBits::e1)
			.setFormat(InFormat)
			.setLoadOp(vk::AttachmentLoadOp::eClear)
			.setStoreOp(vk::AttachmentStoreOp::eStore)
			.setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
			.setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
			.setInitialLayout(vk::ImageLayout::eUndefined)
			.setFinalLayout(vk::ImageLayout::ePresentSrcKHR);

		vk::AttachmentReference ColorAttachmentRef{};
		ColorAttachmentRef
			.setAttachment(0)
			.setLayout(vk::ImageLayout::eColorAttachmentOptimal);

		vk::SubpassDescription SubPass;
		SubPass
			.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
			.setColorAttachments(ColorAttachmentRef)
			.setPDepthStencilAttachment(nullptr);

		vk::RenderPassCreateInfo RenderPassInfo;
		RenderPassInfo
			.setAttachments(ColorAttachment)
			.setSubpasses(SubPass);

		try
		{
			RenderPass = Device.GetHandle().createRenderPass(RenderPassInfo);
			DURIN_TRACE("Vulkan render pass created");
		}
		catch (const std::runtime_error& err)
		{
			DURIN_ERROR("Failed to create vulkan render pass: {}", err.what());
		}
	}

	FVulkanRenderPass::~FVulkanRenderPass()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::RenderPass, RenderPass);
	}

	FVulkanRenderPassManager::FVulkanRenderPassManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanRenderPassManager::~FVulkanRenderPassManager()
	{
		for (FVulkanFramebuffer* Framebuffer : FrameBuffers)
		{
			delete Framebuffer;
		}
		FrameBuffers.clear();
	}

	auto FVulkanRenderPassManager::GetOrCreateRenderPass(FName InRenderPassName, vk::Format InFormat) -> FVulkanRenderPass*
	{
		auto It = RenderPasses.find(InRenderPassName);
		if (It != RenderPasses.end())
		{
			return It->second.get();
		}

		std::shared_ptr<FVulkanRenderPass> RenderPass = std::make_unique<FVulkanRenderPass>(Device, InFormat);
		RenderPasses.emplace(InRenderPassName, RenderPass);
		return RenderPass.get();
	}

	auto FVulkanRenderPassManager::GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo) -> FVulkanFramebuffer*
	{
		// TODO: Support multiple color render targets later
		check(RTInfo.NumColorRenderTargets == 1);
		FVulkanTexture* RT = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[0]);

		for (FVulkanFramebuffer* Framebuffer : FrameBuffers)
		{
			if (Framebuffer->ColorRenderTargetImages[0] == RT->Image)
			{
				return Framebuffer;
			}
		}
		//TODO: Render pass should be selected based on render target layout, but now we just use the first one for simplicity
		FVulkanRenderPass* TestRenderPass = RenderPasses.begin()->second.get();
		FVulkanFramebuffer* Framebuffer = new FVulkanFramebuffer(Device, RTInfo, *TestRenderPass);
		FrameBuffers.push_back(Framebuffer);
		return Framebuffer;
	}

	auto FVulkanRenderPassManager::BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void
	{
		const FClearValueBinding& ClearValue = RPInfo.ColorClearValue;
		check(ClearValue.Binding == EClearBinding::Color);
		const vk::ClearValue VulkanClearValue{{
			ClearValue.ClearValue.Color[0],
			ClearValue.ClearValue.Color[1],
			ClearValue.ClearValue.Color[2],
			ClearValue.ClearValue.Color[3]
		}};
		CmdBuffer->BeginRenderPass(RenderPass, Framebuffer, VulkanClearValue);
	}

	auto FVulkanRenderPassManager::EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void
	{
		InCmdBuffer->EndRenderPass();
	}
}

#include "VulkanRenderPass.h"

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanFramebuffer.h"
#include "VulkanTexture.h"

FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& Device)
	: Device_(Device)
{
	// Color buffer attachement
	vk::AttachmentDescription ColorAttachment;
	ColorAttachment
		.setSamples(vk::SampleCountFlagBits::e1)
		.setFormat(vk::Format::eR8G8B8A8Srgb)
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
		RenderPass_ = Device_.GetHandle().createRenderPass(RenderPassInfo);
		DOGE_DEBUG("Vulkan render pass created");
	}
	catch (const std::runtime_error& err)
	{
		DOGE_ERROR("Failed to create vulkan render pass: {}", err.what());
	}
}

FVulkanRenderPass::~FVulkanRenderPass()
{
	Device_.GetHandle().destroyRenderPass(RenderPass_);
}

FVulkanRenderPassManager::FVulkanRenderPassManager(FVulkanDevice& Device)
	: Device_(Device)
{
}

auto FVulkanRenderPassManager::GetOrCreateRenderPass() -> FVulkanRenderPass*
{
	if (RenderPass_)
	{
		return RenderPass_;
	}

	RenderPass_ = new FVulkanRenderPass(Device_);
	return RenderPass_;
}

auto FVulkanRenderPassManager::GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo) -> FVulkanFramebuffer*
{
	// TODO: Support multiple color render targets later
	check(RTInfo.NumColorRenderTargets == 1);
	FVulkanTexture* RT = static_cast<FVulkanTexture*>(RTInfo.ColorRenderTargets[0]);

	for (FVulkanFramebuffer* Framebuffer : FrameBuffers_)
	{
		if (Framebuffer->ColorRenderTargetImages_[0] == RT->Image_)
		{
			return Framebuffer;
		}
	}
	FVulkanFramebuffer* Framebuffer = new FVulkanFramebuffer(Device_, RTInfo, *RenderPass_);
	FrameBuffers_.push_back(Framebuffer);
	return Framebuffer;
}

auto FVulkanRenderPassManager::BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void
{
	CmdBuffer->BeginRenderPass(RenderPass, Framebuffer);
}

auto FVulkanRenderPassManager::EndRenderPass(FVulkanCommandBuffer* CmdBuffer) -> void
{
	CmdBuffer->EndRenderPass();
}

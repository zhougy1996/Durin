#pragma once

#include "RHIFwd.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFramebuffer;
	class FVulkanCommandBuffer;
	class FVulkanCommandListContext;

	class FVulkanRenderPass
	{
	public:
		FVulkanRenderPass(FVulkanDevice& InDevice, vk::Format InFormat, vk::ImageLayout InFinalLayout, vk::Format InDepthFormat);

		~FVulkanRenderPass();

		auto GetHandle() const -> vk::RenderPass { return RenderPass; }

	private:
		FVulkanDevice& Device;

		vk::RenderPass RenderPass;
	};

	class FVulkanRenderPassManager
	{
	public:
		explicit FVulkanRenderPassManager(FVulkanDevice& InDevice);

		~FVulkanRenderPassManager();

		// TODO: Render pass should be reuse based on render target layout, but now we select by name for simplicity
		auto GetOrCreateRenderPass(FName InRenderPassName, vk::Format Format, vk::Format DepthFormat = vk::Format::eUndefined) -> FVulkanRenderPass*;

		auto GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& RenderPass) -> FVulkanFramebuffer*;

		auto BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, /* const FVulkanRenderTargetLayout& RTLayout,*/ FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void;

		auto EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void;

		auto NotifyDeleted_Image(vk::Image Image) -> void;

	private:
		FVulkanDevice& Device;

		std::unordered_map<FName, std::unique_ptr<FVulkanRenderPass>> RenderPasses;

		std::vector<std::unique_ptr<FVulkanFramebuffer>> FrameBuffers;
	};
}

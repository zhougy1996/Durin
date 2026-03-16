#pragma once

#include "RHIFwd.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFramebuffer;
	class FVulkanCommandBuffer;
	class FVulkanCommandListContext;

	class FVulkanRenderPass
	{
	public:
		FVulkanRenderPass(FVulkanDevice& InDevice, vk::Format InFormat);

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

		// TODO: Render pass should be reuse based on render target layout, but now we select by name for simplicity
		auto GetOrCreateRenderPass(FName InRenderPassName,  vk::Format Format) -> FVulkanRenderPass*;

		auto GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo) -> FVulkanFramebuffer*;

		auto BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, /* const FVulkanRenderTargetLayout& RTLayout,*/ FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void;

		auto EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void;

	private:
		FVulkanDevice& Device;

		std::unordered_map<FName, std::shared_ptr<FVulkanRenderPass>> RenderPasses;

		std::vector<FVulkanFramebuffer*> FrameBuffers;
	};
}
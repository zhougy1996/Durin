#pragma once

#include "RHIDefinitions.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;

	class FVulkanFramebuffer
	{
	public:
		FVulkanFramebuffer(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& InRTInfo, const FVulkanRenderPass& InRenderPass);

		~FVulkanFramebuffer();

		auto GetHandle() const -> vk::Framebuffer { return Framebuffer; }

		auto GetExtent() const -> vk::Extent2D { return Extent; }

	private:
		FVulkanDevice& Device;

		vk::Framebuffer Framebuffer;

		vk::Extent2D Extent;

		std::vector<FVulkanTextureView> AttachmentTextureViews;

		// Logical color render targets
		uint32 NumColorRenderTargets;

		// Actual color attachments required by the render pass, which may be more than the logical color render targets due to multi-sample resolve attachments
		uint32 NumColorAttachments;

		vk::Image ColorRenderTargetImages[MaxSimultaneousRenderTargets];
		vk::Image ColorResolveTargetImages[MaxSimultaneousRenderTargets];
		vk::Image DepthStencilRenderTargetImage;

		friend class FVulkanRenderPassManager;
	};
}
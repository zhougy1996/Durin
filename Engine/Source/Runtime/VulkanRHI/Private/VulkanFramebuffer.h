#pragma once

#include "RHIDefinitions.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;

	// Owns a Vulkan framebuffer compatible with a specific cached render pass.
	class FVulkanFramebuffer
	{
	public:
		FVulkanFramebuffer(FVulkanDevice& InDevice, const FRHIRenderPassInfo& InRPInfo, const FVulkanRenderPass& InRenderPass);

		~FVulkanFramebuffer();

		auto GetHandle() const -> vk::Framebuffer { return Framebuffer; }

		auto GetExtent() const -> vk::Extent2D { return Extent; }

		auto ContainsRenderTarget(vk::Image Image) const -> bool;
		auto IsCompatibleWith(const FVulkanRenderPass& InRenderPass, const FRHIRenderPassInfo& InRPInfo) const -> bool;

	private:
		FVulkanDevice& Device;

		const FVulkanRenderPass* RenderPass = nullptr;

		vk::Framebuffer Framebuffer;

		vk::Extent2D Extent;

		std::vector<FVulkanView> AttachmentTextureViews;

		// How many logical color outputs the pass has
		uint32 NumColorRenderTargets = 0;
		// How many color-related attachment entries the Vulkan framebuffer actually contains, which may be more than NumColorRenderTargets if any of the render targets is also used as a resolve target.
		// NumColorAttachments = NumColorRenderTargets + NumColorResolveTargets, but we store it separately for convenience.
		uint32 NumColorAttachments = 0;

		vk::Image ColorRenderTargetImages[MaxSimultaneousRenderTargets];
		vk::Image ColorResolveTargetImages[MaxSimultaneousRenderTargets];
		vk::Image DepthStencilRenderTargetImage;

		friend class FVulkanRenderPassManager;
	};
}

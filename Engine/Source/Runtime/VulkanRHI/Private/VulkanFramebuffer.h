#pragma once

#include "RHIConstants.h"
#include "VulkanView.h"

struct FRHIRenderTargetsInfo;
class FVulkanDevice;
class FVulkanRenderPass;

class FVulkanFramebuffer
{
public:
	FVulkanFramebuffer(FVulkanDevice& Device, const FRHIRenderTargetsInfo& RTInfo, const FVulkanRenderPass& RenderPass);

	auto GetHandle() -> vk::Framebuffer { return Framebuffer_; }

private:
	FVulkanDevice& Device_;

	vk::Framebuffer Framebuffer_;

	vk::Extent2D Extent_;

	TArray<FVulkanTextureView> AttachmentTextureViews_;

	// Logical color render targets
	uint32 NumColorRenderTargets_;

	// Actual color attachments required by the render pass, which may be more than the logical color render targets due to multi-sample resolve attachments
	uint32 NumColorAttachments_;

	vk::Image ColorRenderTargetImages_[kMaxSimultaneousRenderTargets];
	vk::Image ColorResolveTargetImages_[kMaxSimultaneousRenderTargets];
	vk::Image DepthStencilRenderTargetImage_;

	friend class FVulkanRenderPassManager;
};

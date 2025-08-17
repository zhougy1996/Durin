#pragma once

struct FRHIRenderTargetsInfo;
struct FRHIRenderPassInfo;

class FVulkanDevice;
class FVulkanFramebuffer;
class FVulkanCommandBuffer;
class FVulkanCommandListContext;

class FVulkanRenderPass
{
public:
	FVulkanRenderPass(FVulkanDevice& Device);

	~FVulkanRenderPass();

	auto GetHandle() const -> vk::RenderPass { return RenderPass_; }

private:
	FVulkanDevice& Device_;

	vk::RenderPass RenderPass_;
};

class FVulkanRenderPassManager
{
public:
	FVulkanRenderPassManager(FVulkanDevice& Device);

	auto GetOrCreateRenderPass() -> FVulkanRenderPass*;

	auto GetOrCreateFrameBuffer(const FRHIRenderTargetsInfo& RTInfo) -> FVulkanFramebuffer*;

	auto BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, /* const FVulkanRenderTargetLayout& RTLayout,*/ FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer) -> void;

	auto EndRenderPass(FVulkanCommandBuffer* CmdBuffer) -> void;

private:
	FVulkanDevice& Device_;
	// tmp, use hash later
	FVulkanRenderPass* RenderPass_ = nullptr;

	std::vector<FVulkanFramebuffer*> FrameBuffers_;
};

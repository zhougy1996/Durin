#pragma once

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFramebuffer;
	class FVulkanCommandBuffer;
	class FVulkanCommandListContext;

	// Identifies a cached Vulkan render pass by backend-neutral attachment layout.
	struct FVulkanRenderPassKey
	{
		explicit FVulkanRenderPassKey(const FRHIRenderTargetLayout& InLayout = {});

		auto operator==(const FVulkanRenderPassKey&) const -> bool = default;

		FRHIRenderTargetLayout Layout{};
	};

	// Hashes render-pass compatibility fields for cache lookup.
	struct FVulkanRenderPassKeyHasher
	{
		auto operator()(const FVulkanRenderPassKey& Key) const -> size_t;
	};

	// Owns a Vulkan render pass created for one immutable attachment layout.
	class FVulkanRenderPass
	{
	public:
		FVulkanRenderPass(FVulkanDevice& InDevice, const FVulkanRenderPassKey& InKey);

		~FVulkanRenderPass();

		auto GetHandle() const -> vk::RenderPass { return RenderPass; }
		auto GetKey() const -> const FVulkanRenderPassKey& { return Key; }
		auto GetAttachmentCount() const -> uint32 { return AttachmentCount; }

	private:
		FVulkanDevice& Device;
		FVulkanRenderPassKey Key;

		vk::RenderPass RenderPass;
		uint32 AttachmentCount = 0;
	};

	// Caches compatible render passes and framebuffers for one device.
	class FVulkanRenderPassManager
	{
	public:
		explicit FVulkanRenderPassManager(FVulkanDevice& InDevice);

		~FVulkanRenderPassManager();

		auto GetOrCreateRenderPass(const FRHIRenderTargetLayout& InLayout) -> FVulkanRenderPass*;

		auto GetOrCreateFrameBuffer(const FRHIRenderPassInfo& RPInfo, const FVulkanRenderPass& RenderPass) -> FVulkanFramebuffer*;

		auto BeginRenderPass(FVulkanCommandListContext& Context, FVulkanDevice& Device, FVulkanCommandBuffer* CmdBuffer, const FRHIRenderPassInfo& RPInfo, FVulkanRenderPass* RenderPass, FVulkanFramebuffer* Framebuffer, FName DebugName) -> void;

		auto EndRenderPass(FVulkanCommandBuffer* InCmdBuffer) -> void;

		auto NotifyDeleted_Image(vk::Image Image) -> void;
		// Releases framebuffer-held RHI views before shutdown drains resource deletion.
		auto ReleaseFramebuffers() -> void;

	private:
		FVulkanDevice& Device;

		std::unordered_map<FVulkanRenderPassKey, std::unique_ptr<FVulkanRenderPass>, FVulkanRenderPassKeyHasher> RenderPasses;

		std::vector<std::unique_ptr<FVulkanFramebuffer>> FrameBuffers;
	};
}

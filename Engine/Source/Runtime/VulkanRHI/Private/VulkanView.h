#pragma once

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Owns the optional Vulkan texel-buffer view for one portable counted range.
	class FVulkanBufferView final : public FRHIBufferView
	{
	public:
		FVulkanBufferView(FVulkanDevice& InDevice, FRHIBuffer* InBuffer,
			const FRHIBufferViewDesc& InDesc);
		~FVulkanBufferView() override;

		auto GetHandle() const -> vk::BufferView { return BufferView; }

	private:
		FVulkanDevice& Device;
		vk::BufferView BufferView{};
	};

	// Owns the Vulkan image view for one exact portable texture subresource range.
	class FVulkanTextureView final : public FRHITextureView
	{
	public:
		FVulkanTextureView(FVulkanDevice& InDevice, FRHITexture* InTexture,
			const FRHITextureViewDesc& InDesc);
		~FVulkanTextureView() override;

		auto GetHandle() const -> vk::ImageView { return ImageView; }
		auto GetDebugIdentity() const -> uint64 { return DebugIdentity; }

	private:
		FVulkanDevice& Device;
		vk::ImageView ImageView{};
		uint64 DebugIdentity = 0;
	};

	// Borrows one Vulkan image/view pair owned by a non-RHI aggregate such as a swapchain.
	struct FVulkanView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}

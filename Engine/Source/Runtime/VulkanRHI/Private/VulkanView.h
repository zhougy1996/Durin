#pragma once

#include "CoreMinimal.h"

#include "RHI.h"

#include "VulkanPlatform.h"

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Owns the optional Vulkan texel-buffer view for one portable counted range.
	class FVulkanBufferView final : public FRHIBufferView
	{
	public:
		FVulkanBufferView(FVulkanDevice& InDevice, FRHIBuffer* InBuffer,
			const FRHIBufferViewDesc& InDesc, vk::Result& OutResult);
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
			const FRHITextureViewDesc& InDesc, vk::Result& OutResult);
		FVulkanTextureView(FVulkanDevice& InDevice, FRHITexture* InTexture, const FRHITextureViewDesc& InDesc)
			: FRHITextureView(InTexture, InDesc), Device(InDevice) {}
		FVulkanTextureView(FVulkanDevice& InDevice, FRHITexture* InTexture, const FRHITextureViewDesc& InDesc, vk::ImageView BorrowedView)
			: FRHITextureView(InTexture, InDesc), Device(InDevice), ImageView(BorrowedView), bOwnsImageView(false) {}
		~FVulkanTextureView() override;

		auto MarkUsedByFramebuffer() -> void { bUsedByFramebuffer = true; }
		auto GetHandle() const -> vk::ImageView { return ImageView; }

	private:
		FVulkanDevice& Device;
		vk::ImageView ImageView{};
		bool bOwnsImageView = true;
		bool bUsedByFramebuffer = false;
	};

	// Borrows one Vulkan image/view pair owned by a non-RHI aggregate such as a swapchain.
	struct FVulkanView
	{
		vk::Image Image;
		vk::ImageView ImageView;
		FTextureViewRHIRef SampledView;
	};
}

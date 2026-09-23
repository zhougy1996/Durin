#pragma once

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Owns the optional Vulkan texel-buffer view for one portable counted range.
	class FVulkanBufferView final : public FRHIBufferView
	{
	public:
		static auto Cast(FRHIResource* Resource) -> FVulkanBufferView*
		{
			require(Resource && Resource->GetResourceType() == ERHIResourceType::BufferView
				&& !IsCPUAuthoredBufferResource(Resource));
			return static_cast<FVulkanBufferView*>(Resource);
		}
		static auto Cast(const FRHIResource* Resource) -> const FVulkanBufferView*
		{
			require(Resource && Resource->GetResourceType() == ERHIResourceType::BufferView
				&& !IsCPUAuthoredBufferResource(Resource));
			return static_cast<const FVulkanBufferView*>(Resource);
		}
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
		auto GetSourceImage() const -> vk::Image { return SourceImage; }
		auto GetTextureViewBackingGeneration() const -> uint64
		{
			return TextureViewBackingGeneration;
		}

	private:
		FVulkanDevice& Device;
		vk::ImageView ImageView{};
		vk::Image SourceImage{};
		uint64 DebugIdentity = 0;
		uint64 TextureViewBackingGeneration = 0;
	};

	// Borrows one Vulkan image/view pair owned by a non-RHI aggregate such as a swapchain.
	struct FVulkanView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}

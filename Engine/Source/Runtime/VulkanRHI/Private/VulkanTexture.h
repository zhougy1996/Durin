#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Distinguishes images destroyed by the texture from externally owned swapchain images.
	enum class EImageOwnerType : uint8
	{
		None,
		LocalOwner,
		ExternalOwner,
		Aliased
	};


	// Owns or references a Vulkan image and its default image view.
	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc);

		FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage);

		~FVulkanTexture() override;

		auto GetSubresourceLayout(uint32 MipIndex, uint32 ArrayLayer) const -> vk::ImageLayout;

		auto SetSubresourceLayout(uint32 MipIndex, uint32 ArrayLayer, vk::ImageLayout Layout) -> void;

		auto GetNumMips() const -> uint32 { return NumMips; }

		vk::Image Image{};

		vk::ImageView ImageView{};

		vk::Format Format{};

		ETextureCreateFlags CreateFlags = ETextureCreateFlags::None;

	protected:
		FVulkanDevice& Device;

		FVulkanAllocation Allocation{};

		EImageOwnerType OwnerType = EImageOwnerType::None;

		uint32 NumMips = 1;

		uint32 ArraySize = 1;

		// Layout state follows command recording order so later uploads preserve existing texels.
		std::vector<vk::ImageLayout> SubresourceLayouts;
	};

	// Owns immutable Vulkan sampler state derived from an RHI sampler descriptor.
	class FVulkanSampler : public FRHISampler
	{
	public:
		FVulkanSampler(FVulkanDevice& InDevice, const FRHISamplerDesc& InDesc);
		~FVulkanSampler() override;

		auto GetHandle() const -> vk::Sampler { return Sampler; }

	private:
		FVulkanDevice& Device;
		vk::Sampler Sampler{};
	};

}

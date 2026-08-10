#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"
#include "VulkanResourceState.h"

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


	// Owns or references a Vulkan image; counted view objects own all image views.
	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc);

		FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage);

		~FVulkanTexture() override;

		auto GetStateTracker() -> FVulkanTextureStateTracker& { return StateTracker; }
		auto GetStateTracker() const -> const FVulkanTextureStateTracker& { return StateTracker; }
		auto GetAllocationClass() const -> EVulkanAllocationClassCandidate
		{
			return Allocation.Class;
		}
		VULKANRHI_API auto GetMemoryPropertyFlags() const -> vk::MemoryPropertyFlags;

		vk::Image Image{};

		vk::Format Format{};

		ETextureCreateFlags CreateFlags = ETextureCreateFlags::None;

	protected:
		FVulkanDevice& Device;

		FVulkanAllocation Allocation{};

		EImageOwnerType OwnerType = EImageOwnerType::None;

		FVulkanTextureStateTracker StateTracker;
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

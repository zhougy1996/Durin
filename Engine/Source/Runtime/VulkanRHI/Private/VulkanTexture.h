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
		auto GetDebugName() const -> std::string_view { return DebugName; }
		auto GetViewBackingGeneration() const -> uint64
		{
			return ViewBackingGeneration;
		}
		auto GetAllocationClass() const -> EVulkanAllocationClassCandidate
		{
			return Allocation.Class;
		}
		VULKANRHI_API auto GetMemoryPropertyFlags() const -> vk::MemoryPropertyFlags;

		vk::Image Image{};

		vk::Format Format{};

		ETextureCreateFlags CreateFlags = ETextureCreateFlags::None;

	protected:
		// Selects one image from the current external backing set. The image is
		// part of the automatic-view identity, so cycling swapchain images can
		// reuse one view per image.
		auto SetExternalImage(vk::Image InImage) -> void
		{
			Image = InImage;
		}
		auto AdvanceExternalImageBacking() -> void { ++ViewBackingGeneration; }

		FVulkanDevice& Device;

		FVulkanAllocation Allocation{};

		EImageOwnerType OwnerType = EImageOwnerType::None;

		FVulkanTextureStateTracker StateTracker;
		std::string DebugName;
		uint64 ViewBackingGeneration = 1;
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

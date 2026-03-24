#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanBuffer : public FRHIBuffer
	{
	public:
		FVulkanBuffer(FVulkanDevice* InDevice, const FRHIBufferCreateDesc& InCreateDesc);

		~FVulkanBuffer() override;

	private:
		FVulkanDevice* Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};
	};
} // namespace Doge::VulkanRHI
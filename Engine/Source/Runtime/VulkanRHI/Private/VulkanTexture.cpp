#include "VulkanTexture.h"

#include "VulkanDevice.h"

namespace Doge::VulkanRHI
{
	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: FRHITexture()
		, Image(InImage)
		, Device(InDevice)
	{
	}

	FVulkanTexture::~FVulkanTexture()
	{
	}
} // namespace Doge::VulkanRHI

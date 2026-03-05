#include "VulkanTexture.h"

namespace Doge::VulkanRHI
{
	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image Image)
		: FRHITexture()
		, Image(Image)
		, Device(InDevice)
	{
	}
} // namespace Doge::VulkanRHI

#include "VulkanTexture.h"

namespace Doge::VulkanRHI
{
	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image Image)
		: Device(InDevice)
		, Image(Image)
	{
	}
}

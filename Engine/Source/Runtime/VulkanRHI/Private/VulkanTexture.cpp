#include "VulkanTexture.h"

namespace Doge::VulkanRHI
{
	FVulkanTexture::FVulkanTexture(FVulkanDevice& Device, vk::Image Image)
		: Device_(Device)
		, Image_(Image)
	{
	}
}

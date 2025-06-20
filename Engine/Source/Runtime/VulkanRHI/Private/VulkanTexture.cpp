#include "VulkanTexture.h"

FVulkanTexture::FVulkanTexture(FVulkanDevice& Device, vk::Image Image)
	: Device_(Device)
	, Image_(Image)
{
}

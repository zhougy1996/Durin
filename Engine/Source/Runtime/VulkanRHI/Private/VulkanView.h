#pragma once

namespace Durin::VulkanRHI
{
	// Owns a Vulkan image view and the image whose lifetime it observes.
	struct FVulkanView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}

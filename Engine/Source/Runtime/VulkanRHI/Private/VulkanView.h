#pragma once

namespace Durin::VulkanRHI
{
	struct FVulkanView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}
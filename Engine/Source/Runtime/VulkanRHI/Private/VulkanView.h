#pragma once

namespace Doge::VulkanRHI
{
	struct FVulkanTextureView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}
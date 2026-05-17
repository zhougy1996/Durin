#pragma once

namespace Durin::VulkanRHI
{
	struct FVulkanTextureView
	{
		vk::Image Image;
		vk::ImageView ImageView;
	};
}
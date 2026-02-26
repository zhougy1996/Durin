#pragma once

#include "RHIResources.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& Device, vk::Image Image);

		vk::Image Image_;

		vk::Format Format_;

	protected:
		FVulkanDevice& Device_;
	};
}
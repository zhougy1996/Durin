#pragma once

#include "RHIResources.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage);

		vk::Image Image;

		vk::Format Format;

	protected:
		FVulkanDevice& Device;
	};
}
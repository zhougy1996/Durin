#pragma once

#include "RHIResources.h"

class FVulkanDevice;

class FVulkanTexture : public FRHITexture
{
public:
	FVulkanTexture(FVulkanDevice& Device, vk::Image Image);

	vk::Image Image_;

protected:
	FVulkanDevice& Device_;
};
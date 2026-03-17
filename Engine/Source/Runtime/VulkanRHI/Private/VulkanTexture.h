#pragma once

#include "RHIResources.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc);

		FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage);

		~FVulkanTexture() override;

		vk::Image Image{};

		vk::Format Format{};

	protected:
		FVulkanDevice& Device;
	};
}
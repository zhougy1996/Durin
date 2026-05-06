#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	enum class EImageOwnerType : uint8
	{
		None,
		LocalOwner,
		ExternalOwner,
		Aliased
	};

	class FVulkanTexture : public FRHITexture
	{
	public:
		FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc);

		FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage);

		~FVulkanTexture() override;

		vk::Image Image{};

		vk::ImageView ImageView{};

		vk::Format Format{};

	protected:
		FVulkanDevice& Device;

		FVulkanAllocation Allocation{};

		EImageOwnerType OwnerType = EImageOwnerType::None;
	};
}
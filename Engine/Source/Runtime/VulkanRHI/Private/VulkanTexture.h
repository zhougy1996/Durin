#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"

namespace Durin::VulkanRHI
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

	class FVulkanSampler : public FRHISampler
	{
	public:
		FVulkanSampler(FVulkanDevice& InDevice, const FRHISamplerDesc& InDesc);
		~FVulkanSampler() override;

		auto GetHandle() const -> vk::Sampler { return Sampler; }

	private:
		FVulkanDevice& Device;
		vk::Sampler Sampler{};
	};

}
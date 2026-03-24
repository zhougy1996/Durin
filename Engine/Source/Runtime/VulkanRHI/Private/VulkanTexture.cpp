#include "VulkanTexture.h"

#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"

namespace Doge::VulkanRHI
{
	static auto PickSampleCount(const FRHITextureCreateDesc& InCreateDesc) -> vk::SampleCountFlagBits
	{
		switch (InCreateDesc.NumSamples)
		{
		case 1:
			return vk::SampleCountFlagBits::e1;
		case 2:
			return vk::SampleCountFlagBits::e2;
		case 4:
			return vk::SampleCountFlagBits::e4;
		case 8:
			return vk::SampleCountFlagBits::e8;
		case 16:
			return vk::SampleCountFlagBits::e16;
		default:
			DOGE_ERROR("Unsupported sample count: {}", InCreateDesc.NumSamples);
			return vk::SampleCountFlagBits::e1;
		}
	}

	static auto TextureDimensionToImageViewType(ETextureDimension InDimension) -> vk::ImageViewType
	{
		switch (InDimension)
		{
		case ETextureDimension::Texture2D:
			return vk::ImageViewType::e2D;
		case ETextureDimension::Texture3D:
			return vk::ImageViewType::e3D;
		case ETextureDimension::TextureCube:
			return vk::ImageViewType::eCube;
		case ETextureDimension::Texture2DArray:
			return vk::ImageViewType::e2DArray;
		case ETextureDimension::TextureCubeArray:
			return vk::ImageViewType::eCubeArray;
		default:
			DOGE_ERROR("Unsupported texture dimension");
			return vk::ImageViewType::e2D;
		}
	}

	static auto ConvertToExtent3D(const FIntVector& Size) -> vk::Extent3D
	{
		return vk::Extent3D{static_cast<uint32>(Size.x), static_cast<uint32>(Size.y), static_cast<uint32>(Size.z)};
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc)
		: Device(InDevice)
	{
		vk::Device DeviceHandle = InDevice.GetHandle();

		vk::Format ImageFormat = ConvertToVulkanFormat(InCreateDesc.Format);
		vk::Extent3D ImageExtent = ConvertToExtent3D(InCreateDesc.GetSize());

		vk::ImageCreateInfo imageInfo{};
		imageInfo
			.setImageType(vk::ImageType::e2D)
			.setFormat(ImageFormat)
			.setExtent(ImageExtent)
			.setArrayLayers(InCreateDesc.ArraySize)
			.setMipLevels(InCreateDesc.NumMips)
			.setSamples(PickSampleCount(InCreateDesc))
			.setTiling(vk::ImageTiling::eOptimal)
			.setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
			.setSharingMode(vk::SharingMode::eExclusive)
			.setInitialLayout(vk::ImageLayout::eUndefined);

		FVulkanMemoryManager& MemoryManager = InDevice.GetMemoryManager();
		MemoryManager.CreateImage(Allocation, Image, imageInfo);
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: FRHITexture()
		, Image(InImage)
		, Device(InDevice)
	{
	}

	FVulkanTexture::~FVulkanTexture()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Image, Image, Allocation);
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandList& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture>
	{
		return new FVulkanTexture(*Device, CreateDesc);
	}
} // namespace Doge::VulkanRHI

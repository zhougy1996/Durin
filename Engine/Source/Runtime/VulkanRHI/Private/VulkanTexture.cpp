#include "VulkanTexture.h"

#include "RHICommandList.h"
#include "VulkanBuffer.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanContext.h"
#include "VulkanCommandBuffer.h"

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

	static auto TextureDimensionToImageViewType(ETextureDimension Dimension) -> vk::ImageViewType
	{
		switch (Dimension)
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
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ConvertToVulkanFormat(InCreateDesc.Format))
	{
		vk::Extent3D ImageExtent = ConvertToExtent3D(InCreateDesc.GetSize());

		vk::ImageCreateInfo imageInfo{};
		imageInfo
			.setImageType(vk::ImageType::e2D)
			.setFormat(Format)
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

		// Create default image view
		vk::ImageViewCreateInfo ViewInfo;
		ViewInfo.setImage(Image)
			.setViewType(TextureDimensionToImageViewType(InCreateDesc.Dimension))
			.setFormat(Format)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, InCreateDesc.NumMips, 0, InCreateDesc.ArraySize));

		ImageView = InDevice.GetHandle().createImageView(ViewInfo);
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: Image(InImage)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::ExternalOwner)
	{
	}

	FVulkanTexture::~FVulkanTexture()
	{
		if (OwnerType == EImageOwnerType::LocalOwner)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Image, Image, Allocation);
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ImageView, ImageView);
		}
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture>
	{
		return new FVulkanTexture(*Device, CreateDesc);
	}

	auto FVulkanDynamicRHI::RHIUpdateTexture2D(FRHICommandListBase& RHICmdList, FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData) -> void
	{
		const auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto CmdBuffer = RHIGetVkCommandBuffer(RHICmdList);

		// Create staging buffer
		const uint32 DataSize = UpdateRegion.Height * SourcePitch;
		FStagingBuffer StagingBuffer(*Device, DataSize);
		void* Mapped = StagingBuffer.GetMappedPointer();
		std::memcpy(Mapped, SourceData, DataSize);

		vk::ImageMemoryBarrier PreCopyBarrier;
		PreCopyBarrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
			.setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
			.setOldLayout(vk::ImageLayout::eUndefined)
			.setNewLayout(vk::ImageLayout::eTransferDstOptimal)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, 0, 1));

		CmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, {}, {}, PreCopyBarrier);

		// Copy buffer to image
		uint32 ElementSize = GetFormatElementSize(VulkanTexture->Format);
		vk::BufferImageCopy CopyRegion;
		CopyRegion.setBufferOffset(0)
			.setBufferRowLength(SourcePitch / ElementSize)
			.setBufferImageHeight(0)
			.setImageSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, MipIndex, 0, 1))
			.setImageOffset(vk::Offset3D{static_cast<int32>(UpdateRegion.DestX), static_cast<int32>(UpdateRegion.DestY), 0})
			.setImageExtent(vk::Extent3D{UpdateRegion.Width, UpdateRegion.Height, 1});

		CmdBuffer.copyBufferToImage(StagingBuffer.GetHandle(), VulkanTexture->Image, vk::ImageLayout::eTransferDstOptimal, CopyRegion);

		// Transition image to shader read
		vk::ImageMemoryBarrier PostCopyBarrier;
		PostCopyBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
			.setDstAccessMask(vk::AccessFlagBits::eShaderRead)
			.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
			.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, 0, 1));

		CmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, vk::DependencyFlags{}, {}, {}, PostCopyBarrier);
	}
} // namespace Doge::VulkanRHI

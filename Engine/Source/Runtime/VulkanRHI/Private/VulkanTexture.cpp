#include "VulkanTexture.h"

#include "RHICommandList.h"
#include "VulkanBuffer.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanContext.h"
#include "VulkanCommandBuffer.h"

namespace Durin::VulkanRHI
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
			DURIN_ERROR("Unsupported sample count: {}", InCreateDesc.NumSamples);
			return vk::SampleCountFlagBits::e1;
		}
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc)
		: Device(InDevice)
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ToVulkan_PixelFormat(InCreateDesc.Format))
		, CreateFlags(InCreateDesc.Flags)
	{
		SizeX = static_cast<uint32>(FMath::Max(1, InCreateDesc.Extent.x));
		SizeY = static_cast<uint32>(FMath::Max(1, InCreateDesc.Extent.y));
		vk::Extent3D ImageExtent = ToVulkan_Extent3D(InCreateDesc.GetSize());
		ImageExtent.width = FMath::Max(1u, ImageExtent.width);
		ImageExtent.height = FMath::Max(1u, ImageExtent.height);

		vk::ImageUsageFlags ImageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		if (EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::RenderTargetable))
		{
			ImageUsage |= vk::ImageUsageFlagBits::eColorAttachment;
		}

		vk::ImageCreateInfo imageInfo{};
		imageInfo
			.setImageType(vk::ImageType::e2D)
			.setFormat(Format)
			.setExtent(ImageExtent)
			.setArrayLayers(InCreateDesc.ArraySize)
			.setMipLevels(InCreateDesc.NumMips)
			.setSamples(PickSampleCount(InCreateDesc))
			.setTiling(vk::ImageTiling::eOptimal)
			.setUsage(ImageUsage)
			.setSharingMode(vk::SharingMode::eExclusive)
			.setInitialLayout(vk::ImageLayout::eUndefined);

		FVulkanMemoryManager& MemoryManager = InDevice.GetMemoryManager();
		MemoryManager.CreateImage(Allocation, Image, imageInfo);

		// Create default image view
		vk::ImageViewCreateInfo ViewInfo;
		ViewInfo.setImage(Image)
			.setViewType(ToVulkan_TextureDimension(InCreateDesc.Dimension))
			.setFormat(Format)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, InCreateDesc.NumMips, 0, InCreateDesc.ArraySize));

		ImageView = InDevice.GetHandle().createImageView(ViewInfo);
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: Image(InImage)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::ExternalOwner)
		, CreateFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
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

	FVulkanSampler::FVulkanSampler(FVulkanDevice& InDevice, const FRHISamplerDesc& InDesc)
		: Device(InDevice)
	{
		const float MaxAnisotropy = InDesc.MaxAnisotropy >= 1.0f ? InDesc.MaxAnisotropy : 1.0f;

		vk::SamplerCreateInfo SamplerInfo;
		SamplerInfo.setMagFilter(ToVulkan_SamplerFilter(InDesc.MagFilter))
			.setMinFilter(ToVulkan_SamplerFilter(InDesc.MinFilter))
			.setMipmapMode(ToVulkan_SamplerMipmapMode(InDesc.MipmapMode))
			.setAddressModeU(ToVulkan_SamplerAddressMode(InDesc.AddressU))
			.setAddressModeV(ToVulkan_SamplerAddressMode(InDesc.AddressV))
			.setAddressModeW(ToVulkan_SamplerAddressMode(InDesc.AddressW))
			.setMipLodBias(InDesc.MipLodBias)
			.setAnisotropyEnable(InDesc.bEnableAnisotropy)
			.setMaxAnisotropy(MaxAnisotropy)
			.setCompareEnable(InDesc.bEnableCompare)
			.setCompareOp(ToVulkan_SamplerCompareOp(InDesc.CompareOp))
			.setMinLod(InDesc.MinLod)
			.setMaxLod(InDesc.MaxLod)
			.setBorderColor(ToVulkan_SamplerBorderColor(InDesc.BorderColor))
			.setUnnormalizedCoordinates(InDesc.bUnnormalizedCoordinates);

		Sampler = Device.GetHandle().createSampler(SamplerInfo);
	}

	FVulkanSampler::~FVulkanSampler()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Sampler, Sampler);
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture>
	{
		return new FVulkanTexture(*Device, CreateDesc);
	}

	auto FVulkanDynamicRHI::RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler>
	{
		return new FVulkanSampler(*Device, CreateDesc);
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
} // namespace Durin::VulkanRHI

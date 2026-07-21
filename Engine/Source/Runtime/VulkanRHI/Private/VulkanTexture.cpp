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
			DURIN_ERROR("Unsupported Vulkan texture sample count: value={}.", InCreateDesc.NumSamples);
			return vk::SampleCountFlagBits::e1;
		}
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc)
		: Device(InDevice)
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ToVulkan_PixelFormat(InCreateDesc.Format))
		, CreateFlags(InCreateDesc.Flags)
		, NumMips(InCreateDesc.NumMips)
		, ArraySize(InCreateDesc.ArraySize)
		, SubresourceLayouts(static_cast<size_t>(InCreateDesc.NumMips) * InCreateDesc.ArraySize, vk::ImageLayout::eUndefined)
	{
		SizeX = static_cast<uint32>(FMath::Max(1, InCreateDesc.Extent.x));
		SizeY = static_cast<uint32>(FMath::Max(1, InCreateDesc.Extent.y));
		PixelFormat = InCreateDesc.Format;
		NumSamples = InCreateDesc.NumSamples;
		vk::Extent3D ImageExtent = ToVulkan_Extent3D(InCreateDesc.GetSize());
		ImageExtent.width = FMath::Max(1u, ImageExtent.width);
		ImageExtent.height = FMath::Max(1u, ImageExtent.height);

		const bool bDepthStencil = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bStorage = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::Storage);
		checkf(!bStorage || !bDepthStencil, "Vulkan storage images do not support depth/stencil textures in this RHI");
		checkf(!bStorage || InCreateDesc.NumSamples == 1, "Vulkan storage images must be single-sampled");
		vk::ImageUsageFlags ImageUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		if (EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable))
		{
			ImageUsage |= vk::ImageUsageFlagBits::eColorAttachment;
		}
		if (bDepthStencil)
		{
			ImageUsage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
		}
		if (bStorage)
		{
			ImageUsage |= vk::ImageUsageFlagBits::eStorage;
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
			.setSubresourceRange(vk::ImageSubresourceRange(bDepthStencil ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, InCreateDesc.NumMips, 0, InCreateDesc.ArraySize));

		ImageView = InDevice.GetHandle().createImageView(ViewInfo);
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: Image(InImage)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::ExternalOwner)
		, CreateFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		, SubresourceLayouts(1, vk::ImageLayout::eUndefined)
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

	auto FVulkanTexture::GetSubresourceLayout(uint32 MipIndex, uint32 ArrayLayer) const -> vk::ImageLayout
	{
		check(MipIndex < NumMips && ArrayLayer < ArraySize);
		return SubresourceLayouts[static_cast<size_t>(ArrayLayer) * NumMips + MipIndex];
	}

	auto FVulkanTexture::SetSubresourceLayout(uint32 MipIndex, uint32 ArrayLayer, vk::ImageLayout Layout) -> void
	{
		check(MipIndex < NumMips && ArrayLayer < ArraySize);
		SubresourceLayouts[static_cast<size_t>(ArrayLayer) * NumMips + MipIndex] = Layout;
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
		TRefCountPtr<FVulkanTexture> Texture = new FVulkanTexture(*Device, CreateDesc);
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage))
		{
			// Storage descriptors require GENERAL. Establish it once at creation so an
			// image without initial upload is immediately valid for shader access.
			vk::ImageMemoryBarrier Barrier;
			Barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
				.setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
				.setOldLayout(vk::ImageLayout::eUndefined)
				.setNewLayout(vk::ImageLayout::eGeneral)
				.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
				.setImage(Texture->Image)
				.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, CreateDesc.NumMips, 0, CreateDesc.ArraySize));

			RHIGetVkCommandBuffer(RHICmdList).pipelineBarrier(
				vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eAllGraphics,
				vk::DependencyFlags{}, {}, {}, Barrier);
			for (uint32 ArrayLayer = 0; ArrayLayer < CreateDesc.ArraySize; ++ArrayLayer)
			{
				for (uint32 MipIndex = 0; MipIndex < CreateDesc.NumMips; ++MipIndex)
				{
					Texture->SetSubresourceLayout(MipIndex, ArrayLayer, vk::ImageLayout::eGeneral);
				}
			}
		}
		return Texture;
	}

	auto FVulkanDynamicRHI::RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler>
	{
		return new FVulkanSampler(*Device, CreateDesc);
	}

	auto FVulkanDynamicRHI::RHIUpdateTexture2D(FRHICommandListBase& RHICmdList, FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData) -> void
	{
		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto CmdBuffer = RHIGetVkCommandBuffer(RHICmdList);
		const uint32 ElementSize = GetFormatElementSize(VulkanTexture->Format);

		check(SourceData);
		check(MipIndex < VulkanTexture->GetNumMips());
		const uint32 MipWidth = FMath::Max(1u, VulkanTexture->GetSizeX() >> MipIndex);
		const uint32 MipHeight = FMath::Max(1u, VulkanTexture->GetSizeY() >> MipIndex);
		check(UpdateRegion.SrcX >= 0 && UpdateRegion.SrcY >= 0);
		check(UpdateRegion.Width > 0 && UpdateRegion.Height > 0);
		check(static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width <= MipWidth);
		check(static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height <= MipHeight);
		check((static_cast<uint64>(UpdateRegion.SrcX) + UpdateRegion.Width) * ElementSize <= SourcePitch);

		// Repack only the requested rectangle. SourceData always points at the full source image.
		const uint32 PackedRowPitch = UpdateRegion.Width * ElementSize;
		const uint64 DataSize64 = static_cast<uint64>(UpdateRegion.Height) * PackedRowPitch;
		check(DataSize64 <= std::numeric_limits<uint32>::max());
		const uint32 DataSize = static_cast<uint32>(DataSize64);
		FStagingBuffer StagingBuffer(*Device, DataSize);
		auto* Mapped = static_cast<uint8*>(StagingBuffer.GetMappedPointer());
		const auto* SourceRegion = SourceData + static_cast<size_t>(UpdateRegion.SrcY) * SourcePitch + static_cast<size_t>(UpdateRegion.SrcX) * ElementSize;
		for (uint32 Row = 0; Row < UpdateRegion.Height; ++Row)
		{
			std::memcpy(Mapped + static_cast<size_t>(Row) * PackedRowPitch, SourceRegion + static_cast<size_t>(Row) * SourcePitch, PackedRowPitch);
		}
		StagingBuffer.FlushMappedMemory();

		const vk::ImageLayout OldLayout = VulkanTexture->GetSubresourceLayout(MipIndex, 0);
		const bool bHasPreviousContents = OldLayout != vk::ImageLayout::eUndefined;

		vk::ImageMemoryBarrier PreCopyBarrier;
		const bool bStorage = EnumHasAnyFlags(VulkanTexture->CreateFlags, ETextureCreateFlags::Storage);
		const vk::AccessFlags PreviousAccess = bStorage
			? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
			: vk::AccessFlagBits::eShaderRead;
		PreCopyBarrier.setSrcAccessMask(bHasPreviousContents ? PreviousAccess : vk::AccessFlags{})
			.setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
			.setOldLayout(OldLayout)
			.setNewLayout(vk::ImageLayout::eTransferDstOptimal)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, 0, 1));

		const vk::PipelineStageFlags SourceStage = bHasPreviousContents ? vk::PipelineStageFlagBits::eAllGraphics : vk::PipelineStageFlagBits::eTopOfPipe;
		CmdBuffer.pipelineBarrier(SourceStage, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, {}, {}, PreCopyBarrier);

		// Copy buffer to image
		vk::BufferImageCopy CopyRegion;
		CopyRegion.setBufferOffset(0)
			.setBufferRowLength(0)
			.setBufferImageHeight(0)
			.setImageSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, MipIndex, 0, 1))
			.setImageOffset(vk::Offset3D{static_cast<int32>(UpdateRegion.DestX), static_cast<int32>(UpdateRegion.DestY), 0})
			.setImageExtent(vk::Extent3D{UpdateRegion.Width, UpdateRegion.Height, 1});

		CmdBuffer.copyBufferToImage(StagingBuffer.GetHandle(), VulkanTexture->Image, vk::ImageLayout::eTransferDstOptimal, CopyRegion);

		// Storage-capable images remain GENERAL so sampled and storage descriptors
		// agree on one tracked layout after uploads.
		const vk::ImageLayout FinalLayout = bStorage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal;
		const vk::AccessFlags FinalAccess = bStorage
			? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
			: vk::AccessFlagBits::eShaderRead;
		vk::ImageMemoryBarrier PostCopyBarrier;
		PostCopyBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
			.setDstAccessMask(FinalAccess)
			.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
			.setNewLayout(FinalLayout)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, 0, 1));

		CmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllGraphics, vk::DependencyFlags{}, {}, {}, PostCopyBarrier);
		VulkanTexture->SetSubresourceLayout(MipIndex, 0, FinalLayout);
	}
} // namespace Durin::VulkanRHI

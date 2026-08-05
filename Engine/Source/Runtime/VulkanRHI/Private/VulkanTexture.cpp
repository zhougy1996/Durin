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
		: FRHITexture(InCreateDesc)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ToVulkan_PixelFormat(InCreateDesc.Format))
		, CreateFlags(InCreateDesc.Flags)
		, SubresourceLayouts(static_cast<size_t>(InCreateDesc.NumMips) * InCreateDesc.ArraySize, vk::ImageLayout::eUndefined)
	{
		CheckVulkanRHIThread();
		vk::Extent3D ImageExtent = ToVulkan_Extent3D(InCreateDesc.GetSize());

		const bool bDepthStencil = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bStorage = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::Storage);
		const bool bCube = InCreateDesc.Dimension == ETextureDimension::TextureCube
			|| InCreateDesc.Dimension == ETextureDimension::TextureCubeArray;
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
		if (EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::CPUReadback))
		{
			ImageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
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
		if (bCube) imageInfo.setFlags(vk::ImageCreateFlagBits::eCubeCompatible);

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
		CheckVulkanRHIThread();
		if (OwnerType == EImageOwnerType::LocalOwner)
		{
			constexpr ETextureCreateFlags FramebufferAttachmentFlags =
				ETextureCreateFlags::RenderTargetable |
				ETextureCreateFlags::DepthStencilTargetable |
				ETextureCreateFlags::ResolveTargetable;
			if (EnumHasAnyFlags(CreateFlags, FramebufferAttachmentFlags))
			{
				Device.NotifyDeleted_Image(Image);
			}
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
		CheckVulkanRHIThread();
		check(MipIndex < NumMips && ArrayLayer < ArraySize);
		SubresourceLayouts[static_cast<size_t>(ArrayLayer) * NumMips + MipIndex] = Layout;
	}

	FVulkanSampler::FVulkanSampler(FVulkanDevice& InDevice, const FRHISamplerDesc& InDesc)
		: Device(InDevice)
	{
		CheckVulkanRHIThread();
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
		CheckVulkanRHIThread();
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Sampler, Sampler);
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture>
	{
		std::string ValidationError;
		checkf(ValidateTextureCreateDesc(CreateDesc, ValidationError), "Invalid RHI texture create description: {}", ValidationError);
		TRefCountPtr<FVulkanTexture> Texture;
		if (GRHIThread && !IsInRHIThread())
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, CreateDesc, &Texture]() {
					Texture = new FVulkanTexture(*Device, CreateDesc);
				});
		}
		else
		{
			CheckVulkanRHIThread();
			Texture = new FVulkanTexture(*Device, CreateDesc);
		}
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage))
		{
			RHICmdList.InitializeTexture(Texture.GetReference());
		}
		return Texture;
	}

	auto FVulkanDynamicRHI::InitializeTexture(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture) -> void
	{
		CheckVulkanRHIThread();
		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		check(VulkanTexture);
		if (!EnumHasAnyFlags(VulkanTexture->CreateFlags, ETextureCreateFlags::Storage))
		{
			return;
		}
		vk::ImageMemoryBarrier Barrier;
		Barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
			.setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
			.setOldLayout(vk::ImageLayout::eUndefined)
			.setNewLayout(vk::ImageLayout::eGeneral)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(vk::ImageSubresourceRange(
				vk::ImageAspectFlagBits::eColor, 0,
				VulkanTexture->GetNumMips(), 0, VulkanTexture->GetArraySize()));
		Context.GetCommandBuffer()->GetHandle().pipelineBarrier(
			vk::PipelineStageFlagBits::eTopOfPipe,
			vk::PipelineStageFlagBits::eAllGraphics,
			vk::DependencyFlags{}, {}, {}, Barrier);
		for (uint32 ArrayLayer = 0; ArrayLayer < VulkanTexture->GetArraySize(); ++ArrayLayer)
		{
			for (uint32 MipIndex = 0; MipIndex < VulkanTexture->GetNumMips(); ++MipIndex)
			{
				VulkanTexture->SetSubresourceLayout(
					MipIndex, ArrayLayer, vk::ImageLayout::eGeneral);
			}
		}
	}

	auto FVulkanDynamicRHI::RHIIsTextureFormatSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool
	{
		const vk::Format Format = ToVulkan_PixelFormat(CreateDesc.Format);
		if (Format == vk::Format::eUndefined) return false;

		vk::FormatFeatureFlags RequiredFeatures = vk::FormatFeatureFlagBits::eSampledImage
			| vk::FormatFeatureFlagBits::eTransferDst;
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable))
			RequiredFeatures |= vk::FormatFeatureFlagBits::eColorAttachment;
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable))
			RequiredFeatures |= vk::FormatFeatureFlagBits::eDepthStencilAttachment;
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage))
			RequiredFeatures |= vk::FormatFeatureFlagBits::eStorageImage;
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::CPUReadback))
			RequiredFeatures |= vk::FormatFeatureFlagBits::eTransferSrc;

		const vk::FormatProperties Properties = Device->GetGpu().getFormatProperties(Format);
		return (Properties.optimalTilingFeatures & RequiredFeatures) == RequiredFeatures;
	}

	auto FVulkanDynamicRHI::RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler>
	{
		TRefCountPtr<FRHISampler> Result;
		if (GRHIThread && !IsInRHIThread())
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, CreateDesc, &Result]() {
					Result = new FVulkanSampler(*Device, CreateDesc);
				});
			return Result;
		}
		CheckVulkanRHIThread();
		return new FVulkanSampler(*Device, CreateDesc);
	}

	auto FVulkanDynamicRHI::UpdateTexture2D(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::span<const uint8> SourceData) -> void
	{
		CheckVulkanRHIThread();
		checkf(Texture != nullptr, "RHIUpdateTexture2D requires a texture.");
		checkf(!SourceData.empty(), "RHIUpdateTexture2D requires source data.");
		FRHITextureDesc TextureDesc;
		TextureDesc.Dimension = Texture->GetDimension();
		TextureDesc.Extent = FIntPoint(Texture->GetSizeX(), Texture->GetSizeY());
		TextureDesc.Format = Texture->GetFormat();
		TextureDesc.ArraySize = Texture->GetArraySize();
		TextureDesc.NumMips = Texture->GetNumMips();
		TextureDesc.NumSamples = Texture->GetNumSamples();
		std::string ValidationError;
		checkf(
			ValidateTexture2DUpdate(TextureDesc, MipIndex, ArraySlice, UpdateRegion, SourcePitch, ValidationError),
			"Invalid RHI texture upload: {}",
			ValidationError
		);

		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto CmdBuffer = Context.GetCommandBuffer()->GetHandle();
		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(VulkanTexture->GetFormat());
		const uint32 BlockSize = FormatInfo.BlockSize;
		const uint32 BytesPerBlock = FormatInfo.BytesPerBlock;

		const FPixelFormatLayout PackedLayout = GetPixelFormatLayout(VulkanTexture->GetFormat(), UpdateRegion.Width, UpdateRegion.Height);
		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 SourceBlockY = static_cast<uint32>(UpdateRegion.SrcY) / BlockSize;
		check(PackedLayout.RowPitch <= std::numeric_limits<uint32>::max());
		check(PackedLayout.DataSize <= std::numeric_limits<uint32>::max());
		const uint32 PackedRowPitch = static_cast<uint32>(PackedLayout.RowPitch);
		const uint32 DataSize = static_cast<uint32>(PackedLayout.DataSize);
		FStagingBuffer StagingBuffer(*Device, DataSize);
		auto* Mapped = static_cast<uint8*>(StagingBuffer.GetMappedPointer());
		const auto* SourceRegion = SourceData.data() + SourceBlockY * SourcePitch + SourceBlockX * BytesPerBlock;
		for (uint64 BlockRow = 0; BlockRow < PackedLayout.BlocksHigh; ++BlockRow)
		{
			std::memcpy(Mapped + BlockRow * PackedRowPitch, SourceRegion + BlockRow * SourcePitch, PackedRowPitch);
		}
		StagingBuffer.FlushMappedMemory();

		const vk::ImageLayout OldLayout = VulkanTexture->GetSubresourceLayout(MipIndex, ArraySlice);
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
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, ArraySlice, 1));

		const vk::PipelineStageFlags SourceStage = bHasPreviousContents ? vk::PipelineStageFlagBits::eAllGraphics : vk::PipelineStageFlagBits::eTopOfPipe;
		CmdBuffer.pipelineBarrier(SourceStage, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, {}, {}, PreCopyBarrier);

		// Copy buffer to image
		vk::BufferImageCopy CopyRegion;
		CopyRegion.setBufferOffset(0)
			.setBufferRowLength(0)
			.setBufferImageHeight(0)
			.setImageSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, MipIndex, ArraySlice, 1))
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
			.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, MipIndex, 1, ArraySlice, 1));

		CmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllGraphics, vk::DependencyFlags{}, {}, {}, PostCopyBarrier);
		VulkanTexture->SetSubresourceLayout(MipIndex, ArraySlice, FinalLayout);
	}

	auto FVulkanDynamicRHI::ReadTexture2D(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		std::vector<uint8>& OutData
	) -> bool
	{
		CheckVulkanRHIThread();
		OutData.clear();
		if (Texture == nullptr || MipIndex >= Texture->GetNumMips() || ArraySlice >= Texture->GetArraySize()
			|| Texture->GetNumSamples() != 1)
		{
			DURIN_ERROR("Failed to read Vulkan texture: invalid texture or subresource.");
			return false;
		}

		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		if (!EnumHasAnyFlags(VulkanTexture->CreateFlags, ETextureCreateFlags::CPUReadback))
		{
			DURIN_ERROR("Failed to read Vulkan texture: texture was not created with CPUReadback.");
			return false;
		}

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(Texture->GetFormat());
		if (FormatInfo.BlockSize == 0 || FormatInfo.BytesPerBlock == 0
			|| FormatInfo.bHasDepth || FormatInfo.bHasStencil)
		{
			DURIN_ERROR("Failed to read Vulkan texture: only color formats with a defined block layout are supported.");
			return false;
		}

		const uint32 Width = std::max(1u, Texture->GetSizeX() >> MipIndex);
		const uint32 Height = std::max(1u, Texture->GetSizeY() >> MipIndex);
		const FPixelFormatLayout Layout = GetPixelFormatLayout(Texture->GetFormat(), Width, Height);
		if (Layout.DataSize == 0 || Layout.DataSize > std::numeric_limits<uint32>::max())
		{
			DURIN_ERROR("Failed to read Vulkan texture: subresource size is unsupported.");
			return false;
		}

		FVulkanMemoryManager& MemoryManager = Device->GetMemoryManager();
		FVulkanAllocation ReadbackAllocation;
		vk::Buffer ReadbackBuffer;
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(Layout.DataSize)
			.setUsage(vk::BufferUsageFlagBits::eTransferDst)
			.setSharingMode(vk::SharingMode::eExclusive);
		if (!MemoryManager.CreateBuffer(
			ReadbackAllocation,
			ReadbackBuffer,
			EVulkanAllocationFlags::HostVisible | EVulkanAllocationFlags::PersistentMapped,
			BufferInfo,
			"TextureReadback"))
		{
			return false;
		}

		const vk::ImageLayout OldLayout = VulkanTexture->GetSubresourceLayout(MipIndex, ArraySlice);
		if (OldLayout == vk::ImageLayout::eUndefined)
		{
			DURIN_ERROR("Failed to read Vulkan texture: subresource contents are undefined.");
			MemoryManager.DestroyBuffer(ReadbackAllocation, ReadbackBuffer);
			return false;
		}

		const bool bStorage = OldLayout == vk::ImageLayout::eGeneral;
		const bool bColorAttachment = OldLayout == vk::ImageLayout::eColorAttachmentOptimal;
		const vk::AccessFlags OldAccess = bStorage
			? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
			: bColorAttachment ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eShaderRead;
		const vk::PipelineStageFlags OldStage = bColorAttachment
			? vk::PipelineStageFlagBits::eColorAttachmentOutput : vk::PipelineStageFlagBits::eAllGraphics;
		const vk::ImageSubresourceRange SubresourceRange(
			vk::ImageAspectFlagBits::eColor, MipIndex, 1, ArraySlice, 1);
		vk::ImageMemoryBarrier PreCopyBarrier;
		PreCopyBarrier.setSrcAccessMask(OldAccess)
			.setDstAccessMask(vk::AccessFlagBits::eTransferRead)
			.setOldLayout(OldLayout)
			.setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(SubresourceRange);

		const vk::CommandBuffer CmdBuffer = Context.GetCommandBuffer()->GetHandle();
		CmdBuffer.pipelineBarrier(
			OldStage, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, {}, {}, PreCopyBarrier);
		vk::BufferImageCopy CopyRegion;
		CopyRegion.setBufferOffset(0)
			.setBufferRowLength(0)
			.setBufferImageHeight(0)
			.setImageSubresource(vk::ImageSubresourceLayers(
				vk::ImageAspectFlagBits::eColor, MipIndex, ArraySlice, 1))
			.setImageOffset(vk::Offset3D{0, 0, 0})
			.setImageExtent(vk::Extent3D{Width, Height, 1});
		CmdBuffer.copyImageToBuffer(
			VulkanTexture->Image, vk::ImageLayout::eTransferSrcOptimal, ReadbackBuffer, CopyRegion);

		vk::ImageMemoryBarrier PostCopyBarrier;
		PostCopyBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
			.setDstAccessMask(OldAccess)
			.setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
			.setNewLayout(OldLayout)
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
			.setImage(VulkanTexture->Image)
			.setSubresourceRange(SubresourceRange);
		CmdBuffer.pipelineBarrier(
			vk::PipelineStageFlagBits::eTransfer, OldStage, vk::DependencyFlags{}, {}, {}, PostCopyBarrier);

		Context.Finalize();
		Device->WaitUtilIdle();
		MemoryManager.Invalidate(ReadbackAllocation, 0, Layout.DataSize);
		const auto* MappedData = static_cast<const uint8*>(ReadbackAllocation.GetMappedData());
		if (MappedData == nullptr)
		{
			DURIN_ERROR("Failed to read Vulkan texture: readback allocation is not mapped.");
			MemoryManager.DestroyBuffer(ReadbackAllocation, ReadbackBuffer);
			return false;
		}
		OutData.assign(MappedData, MappedData + Layout.DataSize);
		MemoryManager.DestroyBuffer(ReadbackAllocation, ReadbackBuffer);
		return true;
	}
} // namespace Durin::VulkanRHI

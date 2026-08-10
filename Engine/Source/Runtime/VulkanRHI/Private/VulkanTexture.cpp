#include "VulkanTexture.h"

#include "RHICommandList.h"
#include "VulkanBuffer.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanContext.h"
#include "VulkanCommandBuffer.h"
#include "VulkanView.h"

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

	static auto BuildTextureImageCreateInfo(const FRHITextureCreateDesc& CreateDesc)
		-> vk::ImageCreateInfo
	{
		check(CreateDesc.Dimension == ETextureDimension::Texture2D
			|| CreateDesc.Dimension == ETextureDimension::TextureCube);
		vk::ImageUsageFlags Usage{};
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::ShaderResource))
		{
			Usage |= vk::ImageUsageFlagBits::eSampled;
		}
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DestinationCopy))
		{
			Usage |= vk::ImageUsageFlagBits::eTransferDst;
		}
		if (EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable))
		{
			Usage |= vk::ImageUsageFlagBits::eColorAttachment;
		}
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable))
		{
			Usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
		}
		if (EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage))
		{
			Usage |= vk::ImageUsageFlagBits::eStorage;
		}
		if (EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
		{
			Usage |= vk::ImageUsageFlagBits::eTransferSrc;
		}

		vk::ImageCreateInfo Result;
		Result.setImageType(vk::ImageType::e2D)
			.setFormat(ToVulkan_PixelFormat(CreateDesc.Format))
			.setExtent(ToVulkan_Extent3D(CreateDesc.GetSize()))
			.setArrayLayers(CreateDesc.ArraySize)
			.setMipLevels(CreateDesc.NumMips)
			.setSamples(PickSampleCount(CreateDesc))
			.setTiling(vk::ImageTiling::eOptimal)
			.setUsage(Usage)
			.setSharingMode(vk::SharingMode::eExclusive)
			.setInitialLayout(vk::ImageLayout::eUndefined);
		if (CreateDesc.Dimension == ETextureDimension::TextureCube)
		{
			Result.setFlags(vk::ImageCreateFlagBits::eCubeCompatible);
		}
		return Result;
	}

	static auto NormalizeTextureCreateDesc(FRHITextureCreateDesc Desc) -> FRHITextureCreateDesc
	{
		if (EnumHasAnyFlags(Desc.Flags,
			ETextureCreateFlags::ShaderResource | ETextureCreateFlags::Storage))
		{
			Desc.Flags |= ETextureCreateFlags::DestinationCopy;
		}
		if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::CPUReadback))
		{
			Desc.Flags |= ETextureCreateFlags::SourceCopy;
		}
		return Desc;
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc)
		: FRHITexture(InCreateDesc)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ToVulkan_PixelFormat(InCreateDesc.Format))
		, CreateFlags(InCreateDesc.Flags)
		, StateTracker(InCreateDesc.NumMips, InCreateDesc.ArraySize)
	{
		CheckVulkanRHIThread();
		const vk::Extent3D ImageExtent = ToVulkan_Extent3D(InCreateDesc.GetSize());

		const bool bDepthStencil = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bStorage = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::Storage);
		checkf(!bStorage || !bDepthStencil, "Vulkan storage images do not support depth/stencil textures in this RHI");
		checkf(!bStorage || InCreateDesc.NumSamples == 1, "Vulkan storage images must be single-sampled");
		const vk::ImageCreateInfo ImageInfo = BuildTextureImageCreateInfo(InCreateDesc);

		FVulkanMemoryManager& MemoryManager = InDevice.GetMemoryManager();
		const vk::Result ImageResult =
			MemoryManager.CreateImage(Allocation, Image, ImageInfo);
		if (ImageResult != vk::Result::eSuccess)
		{
			throw std::runtime_error(std::format(
				"Vulkan texture image allocation failed: result={}, extent={}x{}x{}, format={}",
				vk::to_string(ImageResult), ImageExtent.width, ImageExtent.height,
				ImageExtent.depth, vk::to_string(Format)));
		}

	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: Image(InImage)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::ExternalOwner)
		, CreateFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		, StateTracker(1, 1)
	{
		Flags = CreateFlags;
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
			if (Image && EnumHasAnyFlags(CreateFlags, FramebufferAttachmentFlags))
			{
				Device.NotifyDeleted_Image(Image);
			}
			if (Image && Allocation.IsValid())
			{
				Device.GetDeferredDeletionQueue().EnqueueResource(
					FDeferredDeletionQueue::EType::Image, Image, Allocation);
			}
		}
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

#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::Sampler);
#endif
		Sampler = Device.GetHandle().createSampler(SamplerInfo);
	}

	FVulkanSampler::~FVulkanSampler()
	{
		CheckVulkanRHIThread();
		if (Sampler)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::Sampler, Sampler);
		}
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture>
	{
		std::string ValidationError;
		checkf(ValidateTextureCreateDesc(CreateDesc, ValidationError), "Invalid RHI texture create description: {}", ValidationError);
		const FRHITextureCreateDesc NormalizedDesc = NormalizeTextureCreateDesc(CreateDesc);
		if (!RHIIsTextureSupported(NormalizedDesc))
		{
			DURIN_ERROR("Failed to create Vulkan RHI texture '{}': the exact texture description is unsupported.",
				CreateDesc.DebugName ? CreateDesc.DebugName : "<unnamed>");
			return nullptr;
		}
		TRefCountPtr<FVulkanTexture> Texture;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, NormalizedDesc, &Texture]() {
					Texture = new FVulkanTexture(*Device, NormalizedDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI texture '{}': {}",
				CreateDesc.DebugName ? CreateDesc.DebugName : "<unnamed>",
				CreationResult.Diagnostic);
			return nullptr;
		}
		if (EnumHasAnyFlags(NormalizedDesc.Flags, ETextureCreateFlags::Storage))
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
		const std::array Transition{FRHITextureTransition::Whole(
			VulkanTexture, ERHIAccess::Discard, ERHIAccess::GraphicsShaderReadWrite)};
		Context.RHITransitionTextures(Transition);
	}

	auto FVulkanDynamicRHI::RHIIsTextureSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool
	{
		std::string ValidationError;
		checkf(ValidateTextureCreateDesc(CreateDesc, ValidationError),
			"Invalid RHI texture create description: {}", ValidationError);
		const FRHICapabilities* Capabilities = RHIGetCapabilities();
		if (Capabilities == nullptr) return false;
		const bool bTexture2D = CreateDesc.Dimension == ETextureDimension::Texture2D;
		const bool bTextureCube = CreateDesc.Dimension == ETextureDimension::TextureCube;
		if ((!bTexture2D && !bTextureCube)
			|| (bTexture2D && !EnumHasAnyFlags(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::Texture2D))
			|| (bTextureCube && !EnumHasAnyFlags(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::TextureCube)))
		{
			return false;
		}
		const uint32 DimensionLimit = bTextureCube
			? Capabilities->MaxTextureDimensionCube : Capabilities->MaxTextureDimension2D;
		if (static_cast<uint32>(CreateDesc.Extent.x) > DimensionLimit
			|| static_cast<uint32>(CreateDesc.Extent.y) > DimensionLimit
			|| CreateDesc.ArraySize > Capabilities->MaxTextureArrayLayers)
		{
			return false;
		}

		const vk::ImageCreateInfo ImageInfo = BuildTextureImageCreateInfo(
			NormalizeTextureCreateDesc(CreateDesc));
		try
		{
			const vk::ImageFormatProperties Properties = Device->GetGpu().getImageFormatProperties(
				ImageInfo.format, ImageInfo.imageType, ImageInfo.tiling, ImageInfo.usage, ImageInfo.flags);
			if (ImageInfo.extent.width > Properties.maxExtent.width
				|| ImageInfo.extent.height > Properties.maxExtent.height
				|| ImageInfo.extent.depth > Properties.maxExtent.depth
				|| ImageInfo.mipLevels > Properties.maxMipLevels
				|| ImageInfo.arrayLayers > Properties.maxArrayLayers
				|| !(Properties.sampleCounts & ImageInfo.samples))
			{
				return false;
			}
			uint64 PayloadSize = 0;
			for (uint32 MipIndex = 0; MipIndex < CreateDesc.NumMips; ++MipIndex)
			{
				const uint32 Width = std::max(1u, static_cast<uint32>(CreateDesc.Extent.x) >> MipIndex);
				const uint32 Height = std::max(1u, static_cast<uint32>(CreateDesc.Extent.y) >> MipIndex);
				const uint64 MipSize = GetPixelFormatLayout(CreateDesc.Format, Width, Height).DataSize;
				if (MipSize > std::numeric_limits<uint64>::max() - PayloadSize) return false;
				PayloadSize += MipSize;
			}
			if (PayloadSize > std::numeric_limits<uint64>::max() / CreateDesc.ArraySize) return false;
			return PayloadSize * CreateDesc.ArraySize <= Properties.maxResourceSize;
		}
		catch (const vk::SystemError&)
		{
			return false;
		}
	}

	auto FVulkanDynamicRHI::RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler>
	{
		TRefCountPtr<FRHISampler> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, CreateDesc, &Result]() {
					Result = new FVulkanSampler(*Device, CreateDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI sampler: {}",
				CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}

	auto FVulkanDynamicRHI::RHICreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef
	{
		std::string Error;
		if (!ValidateBufferViewDesc(Buffer, Desc, Error))
		{
			DURIN_ERROR("Failed to create Vulkan buffer view: {}", Error);
			return nullptr;
		}
		if (Desc.Type == ERHIBufferViewType::Formatted)
		{
			const vk::FormatProperties Properties = Device->GetGpu().getFormatProperties(
				ToVulkan_PixelFormat(Desc.Format));
			if (!(Properties.bufferFeatures & (vk::FormatFeatureFlagBits::eUniformTexelBuffer
				| vk::FormatFeatureFlagBits::eStorageTexelBuffer)))
			{
				DURIN_ERROR("Failed to create Vulkan formatted buffer view: format has no texel-buffer support.");
				return nullptr;
			}
		}
		FBufferViewRHIRef Result;
		const FRHIFallibleOperationResult CreationResult = ExecuteFallibleVulkanCreationOperation(
			[this, Buffer, Desc, &Result]() {
				Result = new FVulkanBufferView(*Device, Buffer, Desc);
			});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan buffer view: {}", CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}

	auto FVulkanDynamicRHI::RHICreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef
	{
		std::string Error;
		if (!ValidateTextureViewDesc(Texture, Desc, Error))
		{
			DURIN_ERROR("Failed to create Vulkan texture view: {}", Error);
			return nullptr;
		}
		FTextureViewRHIRef Result;
		const FRHIFallibleOperationResult CreationResult = ExecuteFallibleVulkanCreationOperation(
			[this, Texture, Desc, &Result]() {
				Result = new FVulkanTextureView(*Device, Texture, Desc);
			});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan texture view: {}", CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
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
		FRHIBufferCreateDesc StagingDesc = FRHIBufferCreateDesc::Create(
			"TextureUploadStaging", DataSize, 0,
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::SourceCopy);
		TRefCountPtr<FVulkanBuffer> StagingBuffer = new FVulkanBuffer(*Device, StagingDesc);
		auto* Mapped = static_cast<uint8*>(StagingBuffer->GetMappedPointer());
		const auto* SourceRegion = SourceData.data() + SourceBlockY * SourcePitch + SourceBlockX * BytesPerBlock;
		for (uint64 BlockRow = 0; BlockRow < PackedLayout.BlocksHigh; ++BlockRow)
		{
			std::memcpy(Mapped + BlockRow * PackedRowPitch, SourceRegion + BlockRow * SourcePitch, PackedRowPitch);
		}
		StagingBuffer->FlushMappedMemory();
		StagingBuffer->GetStateTracker().Apply(0, DataSize, ERHIAccess::HostWrite);
		const std::array StagingTransition{FRHIBufferTransition{
			StagingBuffer.GetReference(), 0, DataSize,
			ERHIAccess::HostWrite, ERHIAccess::TransferRead}};
		Context.RHITransitionBuffers(StagingTransition);

		const bool bStorage = EnumHasAnyFlags(VulkanTexture->CreateFlags, ETextureCreateFlags::Storage);
		const FRHITextureSubresourceRange TransitionRange{
			ERHITextureAspect::Color, MipIndex, 1, ArraySlice, 1};
		const ERHIAccess PreviousAccess = VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, MipIndex, ArraySlice);
		const std::array PreCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, PreviousAccess, ERHIAccess::TransferWrite}};
		Context.RHITransitionTextures(PreCopyTransition);

		const std::array CopyRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = 0,
			.TextureAspect = ERHITextureAspect::Color,
			.TextureMip = MipIndex,
			.TextureFirstArrayLayer = ArraySlice,
			.TextureNumArrayLayers = 1,
			.TextureOffset = {static_cast<int32>(UpdateRegion.DestX),
				static_cast<int32>(UpdateRegion.DestY), 0},
			.TextureExtent = {UpdateRegion.Width, UpdateRegion.Height, 1}}};
		Context.RHICopyBufferToTexture(StagingBuffer.GetReference(), VulkanTexture, CopyRegions);

		const ERHIAccess FinalAccess = bStorage
			? ERHIAccess::GraphicsShaderReadWrite : ERHIAccess::GraphicsShaderRead;
		const std::array PostCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, ERHIAccess::TransferWrite, FinalAccess}};
		Context.RHITransitionTextures(PostCopyTransition);
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

		const ERHIAccess OldAccess = VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, MipIndex, ArraySlice);
		if (OldAccess == ERHIAccess::None)
		{
			DURIN_ERROR("Failed to read Vulkan texture: subresource contents are undefined.");
			return false;
		}
		FRHIBufferCreateDesc ReadbackDesc = FRHIBufferCreateDesc::Create(
			"TextureReadback", static_cast<uint32>(Layout.DataSize), 0,
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::KeepCPUAccessible
				| EBufferUsageFlags::DestinationCopy);
		TRefCountPtr<FVulkanBuffer> ReadbackBuffer = new FVulkanBuffer(*Device, ReadbackDesc);

		const FRHITextureSubresourceRange TransitionRange{
			ERHITextureAspect::Color, MipIndex, 1, ArraySlice, 1};
		const std::array PreCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, OldAccess, ERHIAccess::TransferRead}};
		Context.RHITransitionTextures(PreCopyTransition);
		const std::array ReadbackTransition{FRHIBufferTransition{
			ReadbackBuffer.GetReference(), 0, Layout.DataSize,
			ERHIAccess::Discard, ERHIAccess::TransferWrite}};
		Context.RHITransitionBuffers(ReadbackTransition);
		const std::array CopyRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = 0,
			.TextureAspect = ERHITextureAspect::Color,
			.TextureMip = MipIndex,
			.TextureFirstArrayLayer = ArraySlice,
			.TextureNumArrayLayers = 1,
			.TextureExtent = {Width, Height, 1}}};
		Context.RHICopyTextureToBuffer(VulkanTexture, ReadbackBuffer.GetReference(), CopyRegions);

		const std::array PostCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, ERHIAccess::TransferRead, OldAccess}};
		Context.RHITransitionTextures(PostCopyTransition);
		const std::array HostTransition{FRHIBufferTransition{
			ReadbackBuffer.GetReference(), 0, Layout.DataSize,
			ERHIAccess::TransferWrite, ERHIAccess::HostRead}};
		Context.RHITransitionBuffers(HostTransition);

		Context.Finalize();
		Device->WaitUtilIdle();
		ReadbackBuffer->InvalidateMappedMemory(0, static_cast<uint32>(Layout.DataSize));
		const auto* MappedData = static_cast<const uint8*>(ReadbackBuffer->GetMappedPointer());
		if (MappedData == nullptr)
		{
			DURIN_ERROR("Failed to read Vulkan texture: readback allocation is not mapped.");
			return false;
		}
		OutData.assign(MappedData, MappedData + Layout.DataSize);
		return true;
	}
} // namespace Durin::VulkanRHI

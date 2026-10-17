#include "Backend/RHITextureUpload.h"
#include "Backend/RHIValidation.h"
#include "Backend/RHICompletionBackend.h"
#include "VulkanTexture.h"
#include "VulkanSubmission.h"

#include "RHICommandList.h"
#include "VulkanBuffer.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanMemory.h"
#include "VulkanContext.h"
#include "VulkanCommandBuffer.h"
#include "VulkanView.h"
#include "VulkanViewCache.h"

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
			|| CreateDesc.Dimension == ETextureDimension::Texture2DArray
			|| CreateDesc.Dimension == ETextureDimension::Texture3D
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
		Result.setImageType(CreateDesc.Dimension == ETextureDimension::Texture3D
				? vk::ImageType::e3D : vk::ImageType::e2D)
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

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, const FRHITextureCreateDesc& InCreateDesc,
		vk::Result* OutResult, FVulkanTexture* AliasSource)
		: FRHITexture(InCreateDesc)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::LocalOwner)
		, Format(ToVulkan_PixelFormat(InCreateDesc.Format))
		, CreateFlags(InCreateDesc.Flags)
		, StateTracker(InCreateDesc.NumMips, InCreateDesc.ArraySize)
		, DebugName(Device.GetRHI().GetDebugUtils().CanNameObjects()
			? (InCreateDesc.DebugName ? std::string(InCreateDesc.DebugName)
				: Device.GetRHI().GetDebugUtils().MakeInternalName("Image")) : std::string{})
	{
		const bool bDepthStencil = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bStorage = EnumHasAnyFlags(InCreateDesc.Flags, ETextureCreateFlags::Storage);
		checkf(!bStorage || !bDepthStencil, "Vulkan storage images do not support depth/stencil textures in this RHI");
		checkf(!bStorage || InCreateDesc.NumSamples == 1, "Vulkan storage images must be single-sampled");
		bAllowAliasing = InCreateDesc.Aliasing == ERHITransientAliasing::Enabled || AliasSource != nullptr;
		auto ImageInfo = BuildTextureImageCreateInfo(InCreateDesc);
		if (bAllowAliasing || AliasSource) ImageInfo.flags |= vk::ImageCreateFlagBits::eAlias;

		FVulkanMemoryManager& MemoryManager = InDevice.GetMemoryManager();
		vk::Result ImageResult;
		if (AliasSource)
		{
			require(AliasSource->bAllowAliasing && !AliasSource->HasExternalBacking() && &AliasSource->Device == &Device);
			AliasOwner = AliasSource->AliasOwner ? AliasSource->AliasOwner.GetReference() : AliasSource;
			Allocation = AliasSource->Allocation;
			ImageResult = MemoryManager.CreateAliasingImage(Allocation, ImageInfo, Image);
		}
		else ImageResult = MemoryManager.CreateImage(Allocation, Image,
			EVulkanAllocationClassCandidate::DeviceLocal, ImageInfo, DebugName.c_str(), bAllowAliasing);
		if (OutResult) *OutResult = ImageResult;
		if (ImageResult != vk::Result::eSuccess)
		{
			Image = nullptr;
			Allocation = {};
			if (!OutResult) ThrowVulkanError(ImageResult, "Vulkan texture allocation");
			return;
		}
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Image, [&] { return std::string_view(DebugName); });
	}

	FVulkanTexture::FVulkanTexture(FVulkanDevice& InDevice, vk::Image InImage)
		: Image(InImage)
		, Device(InDevice)
		, OwnerType(EImageOwnerType::ExternalOwner)
		, CreateFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		, StateTracker(1, 1)
		, DebugName(Device.GetRHI().GetDebugUtils().CanNameObjects()
			? Device.GetRHI().GetDebugUtils().MakeInternalName("SwapchainImage") : std::string{})
	{
		Flags = CreateFlags;
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Image, [&] { return std::string_view(DebugName); });
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
			if (Image && AliasOwner)
			{
				Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Image, Image);
			}
			else if (Image && Allocation.IsValid())
			{
				Device.GetDeferredDeletionQueue().EnqueueResource(
					FDeferredDeletionQueue::EType::Image, Image, Allocation);
			}
		}
	}

	auto FVulkanTexture::GetMemoryPropertyFlags() const
		-> vk::MemoryPropertyFlags
	{
		return Device.GetMemoryManager().GetMemoryType(Allocation).propertyFlags;
	}

	FVulkanSampler::FVulkanSampler(FVulkanDevice& InDevice, const FRHISamplerDesc& InDesc, vk::Result& OutResult)
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

		OutResult = Device.GetHandle().createSampler(&SamplerInfo, nullptr, &Sampler);
		if (OutResult != vk::Result::eSuccess) { Sampler = nullptr; return; }
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Sampler,
			[&] { return Device.GetRHI().GetDebugUtils().MakeInternalName("Sampler"); });
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

	auto FVulkanDynamicRHI::RHICreateAliasedTexture(const FRHITextureCreateDesc& Desc, FRHITexture* Source) -> FTextureRHIRef
	{
		if (!Source) return {};
		const auto Normalized = NormalizeTextureCreateDesc(Desc);
		const auto* NativeSource = static_cast<FVulkanTexture*>(Source);
		if (!NativeSource->IsAliasable() || !IsNormalizedTextureSupported(Normalized)
			|| Source->GetDimension() != Normalized.Dimension || Source->GetFlags() != Normalized.Flags
			|| Source->GetSizeX() != uint32(Normalized.Extent.x) || Source->GetSizeY() != uint32(Normalized.Extent.y)
			|| Source->GetSizeZ() != Normalized.Depth || Source->GetArraySize() != Normalized.ArraySize
			|| Source->GetNumMips() != Normalized.NumMips || Source->GetNumSamples() != Normalized.NumSamples
			|| Source->GetFormat() != Normalized.Format) return {};
		vk::Result Result;
		FTextureRHIRef Texture = new FVulkanTexture(*Device, Normalized, &Result,
			static_cast<FVulkanTexture*>(Source));
		if (Result != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(Result)) ThrowVulkanError(Result, "Vulkan aliased texture allocation");
			LogVulkanCreationFailure(Result, "aliased texture", Desc.DebugName ? Desc.DebugName : "");
		}
		return Result == vk::Result::eSuccess ? Texture : FTextureRHIRef{};
	}

	auto FVulkanDynamicRHI::RHICreateTexture(FRHICommandListBase& RHICmdList,
		const FRHITextureCreateDesc& CreateDesc)
		-> FTextureRHIRef
	{
#if DO_CHECK
		const auto ValidationResult = ValidateTextureCreateDesc(CreateDesc);
		checkf(ValidationResult,
			"Invalid RHI texture create description: {}", ToString(ValidationResult.error()));
#endif
		const FRHITextureCreateDesc NormalizedDesc = NormalizeTextureCreateDesc(CreateDesc);
		if (!IsNormalizedTextureSupported(NormalizedDesc))
		{
			DURIN_ERROR("Failed to create Vulkan texture '{}': unsupported format or usage",
				CreateDesc.DebugName ? CreateDesc.DebugName : "<unnamed>");
			return {};
		}
		vk::Result CreationResult;
		FTextureRHIRef Texture = new FVulkanTexture(*Device, NormalizedDesc, &CreationResult);
		if (CreationResult != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(CreationResult))
				ThrowVulkanError(CreationResult, "Vulkan texture allocation");
			LogVulkanCreationFailure(CreationResult, "texture", CreateDesc.DebugName ? CreateDesc.DebugName : "");
			return {};
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
#if DO_CHECK
		const auto ValidationResult = ValidateTextureCreateDesc(CreateDesc);
		checkf(ValidationResult,
			"Invalid RHI texture create description: {}", ToString(ValidationResult.error()));
#endif
		return IsNormalizedTextureSupported(NormalizeTextureCreateDesc(CreateDesc));
	}

	auto FVulkanDynamicRHI::IsNormalizedTextureSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool
	{
		const FRHICapabilities* Capabilities = RHIGetCapabilities();
		if (Capabilities == nullptr) return false;
		const bool bTexture2D = CreateDesc.Dimension == ETextureDimension::Texture2D;
		const bool bTexture2DArray =
			CreateDesc.Dimension == ETextureDimension::Texture2DArray;
		const bool bTexture3D = CreateDesc.Dimension == ETextureDimension::Texture3D;
		const bool bTextureCube = CreateDesc.Dimension == ETextureDimension::TextureCube;
		if ((!bTexture2D && !bTexture2DArray && !bTexture3D && !bTextureCube)
			|| (bTexture2D && !EnumHasAnyFlags(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::Texture2D))
			|| (bTexture2DArray && !EnumHasAnyFlags(
				Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::Texture2DArray))
			|| (bTexture3D && !EnumHasAnyFlags(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::Texture3D))
			|| (bTextureCube && !EnumHasAnyFlags(Capabilities->SupportedTextureDimensions,
				ERHITextureDimensionFlags::TextureCube)))
		{
			return false;
		}
		const uint32 DimensionLimit = bTexture3D
			? Capabilities->MaxTextureDimension3D
			: (bTextureCube ? Capabilities->MaxTextureDimensionCube
				: Capabilities->MaxTextureDimension2D);
		if (static_cast<uint32>(CreateDesc.Extent.x) > DimensionLimit
			|| static_cast<uint32>(CreateDesc.Extent.y) > DimensionLimit
			|| (bTexture3D && CreateDesc.Depth > DimensionLimit)
			|| (!bTexture3D && CreateDesc.ArraySize > Capabilities->MaxTextureArrayLayers))
		{
			return false;
		}

		const vk::ImageCreateInfo ImageInfo = BuildTextureImageCreateInfo(CreateDesc);
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
				const uint32 Depth = bTexture3D
					? std::max(1u, static_cast<uint32>(CreateDesc.Depth) >> MipIndex) : 1u;
				const uint64 SliceSize = GetPixelFormatLayout(CreateDesc.Format, Width, Height).DataSize;
				if (SliceSize > std::numeric_limits<uint64>::max() / Depth) return false;
				const uint64 MipSize = SliceSize * Depth;
				if (MipSize > std::numeric_limits<uint64>::max() - PayloadSize) return false;
				PayloadSize += MipSize;
			}
			if (PayloadSize > std::numeric_limits<uint64>::max() / CreateDesc.ArraySize) return false;
			return PayloadSize * CreateDesc.ArraySize <= Properties.maxResourceSize;
		}
		catch (const vk::SystemError& Exception)
		{
			if (!IsRecoverableVulkanCreationError(
					static_cast<vk::Result>(Exception.code().value()))) throw;
			return false;
		}
	}

	auto FVulkanDynamicRHI::RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler>
	{
		vk::Result CreationResult;
		TRefCountPtr<FRHISampler> Sampler = new FVulkanSampler(*Device, CreateDesc, CreationResult);
		if (CreationResult != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(CreationResult))
				ThrowVulkanError(CreationResult, "Vulkan sampler creation");
			LogVulkanCreationFailure(CreationResult, "sampler");
			return nullptr;
		}
		return Sampler;
	}

	auto FVulkanDynamicRHI::RHICreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef
	{

		const auto ValidationResult = ValidateBufferViewDesc(Buffer, Desc);
		if (!ValidationResult)
		{
			DURIN_ERROR("Failed to create Vulkan buffer view: {}", ToString(ValidationResult.error()));
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
		vk::Result CreationResult;
		FBufferViewRHIRef View = new FVulkanBufferView(*Device, Buffer, Desc, &CreationResult);
		if (CreationResult != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(CreationResult))
				ThrowVulkanError(CreationResult, "Vulkan buffer view creation");
			LogVulkanCreationFailure(CreationResult, "buffer view");
			return nullptr;
		}
		return View;
	}

	auto FVulkanDynamicRHI::RHICreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef
	{
		const auto ValidationResult = ValidateTextureViewDesc(Texture, Desc);
		if (!ValidationResult)
		{
			DURIN_ERROR("Failed to create Vulkan texture view: {}", ToString(ValidationResult.error()));
			return nullptr;
		}
		FTextureViewRHIRef Result;
		auto CreationOperation = [this, Texture, Desc, &Result
		]() {
			vk::Result CreationResult;
			FTextureViewRHIRef Candidate = new FVulkanTextureView(*Device, Texture, Desc, CreationResult);
			if (CreationResult != vk::Result::eSuccess)
			{
				if (!IsRecoverableVulkanCreationError(CreationResult))
					ThrowVulkanError(CreationResult, "Vulkan texture view creation");
				LogVulkanCreationFailure(CreationResult, "texture view");
				return;
			}
			Result = std::move(Candidate);
		};
		// Swapchain backing changes on replay; only stable local images are caller-domain factories.
		if (static_cast<FVulkanTexture*>(Texture)->HasExternalBacking() && GRHIThread && !IsInRHIThread())
			GCommandListExecutor.ExecuteSynchronousOperation(false, std::move(CreationOperation));
		else CreationOperation();
		return Result;
	}

	auto FVulkanDynamicRHI::RHIGetOrCreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef
	{
		const uint64 FrameNumber = GCommandListExecutor.GetFrameNumber();
		if (FBufferViewRHIRef Cached = ViewCache->FindBufferView(
			Buffer, Desc, FrameNumber))
		{
			return Cached;
		}
		FBufferViewRHIRef Candidate = RHICreateBufferView(Buffer, Desc);
		return Candidate
			? ViewCache->PublishBufferView(std::move(Candidate), FrameNumber)
			: nullptr;
	}

	auto FVulkanDynamicRHI::RHIGetOrCreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef
	{
		const uint64 FrameNumber = GCommandListExecutor.GetFrameNumber();
		if (FTextureViewRHIRef Cached = ViewCache->FindTextureView(
			Texture, Desc, FrameNumber))
		{
			return Cached;
		}
		FTextureViewRHIRef Candidate = RHICreateTextureView(Texture, Desc);
		return Candidate
			? ViewCache->PublishTextureView(std::move(Candidate), FrameNumber)
			: nullptr;
	}

	auto FVulkanDynamicRHI::UpdateTexture2D(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		FByteView SourceData) -> void
	{
		CheckVulkanRHIThread();
		checkf(Texture != nullptr, "RHIUpdateTexture2D requires a texture.");
		checkf(!SourceData.empty(), "RHIUpdateTexture2D requires source data.");
		const FRHITextureDesc TextureDesc = GetTextureUploadDesc(*Texture);
#if DO_CHECK
		const auto ValidationResult = ValidateTexture2DUpdate(TextureDesc, MipIndex, ArraySlice, UpdateRegion, SourcePitch);
		checkf(ValidationResult,
			"Invalid RHI texture upload: {}",
			ToString(ValidationResult.error()));
#endif

		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto& FormatInfo = GetPixelFormatInfo(VulkanTexture->GetFormat());
		const uint32 BytesPerBlock = FormatInfo.BytesPerBlock;
		const FUpdateTextureRegion3D SourceRegion3D{UpdateRegion.DestX, UpdateRegion.DestY, 0,
			UpdateRegion.SrcX, UpdateRegion.SrcY, 0, UpdateRegion.Width, UpdateRegion.Height, 1};
		const auto Layout = ComputeTextureUploadLayout(VulkanTexture->GetFormat(),
			SourceRegion3D, SourcePitch, 0, SourceData.size());
		requiref(Layout, "Invalid Vulkan upload source: {}", ToString(Layout.error()));
		const uint32 DataSize = Layout->Size;
		const uint64 Alignment = std::max<uint64>({16, BytesPerBlock,
			Device->GetGpuProperties().limits.nonCoherentAtomSize,
			Device->GetGpuProperties().limits.optimalBufferCopyOffsetAlignment});
		FVulkanTransferRange Staging = Context.AcquireTransferRange(
			EVulkanAllocationClassCandidate::TransferUpload, DataSize, Alignment);
		FVulkanBuffer* StagingBuffer = Staging.GetBuffer();
		GVulkanMemoryBaselineTracker.RecordUpload(DataSize);
		auto* Mapped = Staging.GetMappedPointer();
		CopyTextureUploadData(*Layout, SourceData, {Mapped, DataSize});
		Staging.Flush();
		StagingBuffer->GetStateTracker().Apply(
			Staging.GetOffset(), DataSize, ERHIAccess::HostWrite);
		const std::array StagingTransition{FRHIBufferTransition{
			StagingBuffer, Staging.GetOffset(), DataSize,
			ERHIAccess::HostWrite, ERHIAccess::TransferRead}};
		Context.RHITransitionBuffers(StagingTransition);

		const FRHITextureSubresourceRange TransitionRange{
			ERHITextureAspect::Color, MipIndex, 1, ArraySlice, 1};
		const ERHIAccess PreviousAccess = VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, MipIndex, ArraySlice);
		const std::array PreCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, PreviousAccess, ERHIAccess::TransferWrite}};
		Context.RHITransitionTextures(PreCopyTransition);

		const std::array CopyRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = Staging.GetOffset(),
			.TextureAspect = ERHITextureAspect::Color,
			.TextureMip = MipIndex,
			.TextureFirstArrayLayer = ArraySlice,
			.TextureNumArrayLayers = 1,
			.TextureOffset = {static_cast<int32>(UpdateRegion.DestX),
				static_cast<int32>(UpdateRegion.DestY), 0},
			.TextureExtent = {UpdateRegion.Width, UpdateRegion.Height, 1}}};
		Context.RHICopyBufferToTexture(StagingBuffer, VulkanTexture, CopyRegions);

		Staging.Retire();
	}

	auto FVulkanDynamicRHI::UpdateTexture3D(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture,
		uint32 MipIndex,
		const FUpdateTextureRegion3D& UpdateRegion,
		uint32 SourceRowPitch,
		uint32 SourceDepthPitch,
		FByteView SourceData) -> void
	{
		CheckVulkanRHIThread();
		checkf(Texture != nullptr, "RHIUpdateTexture3D requires a texture.");
		checkf(!SourceData.empty(), "RHIUpdateTexture3D requires source data.");
		const FRHITextureDesc TextureDesc = GetTextureUploadDesc(*Texture);
#if DO_CHECK
		const auto ValidationResult = ValidateTexture3DUpdate(TextureDesc, MipIndex, UpdateRegion,
			SourceRowPitch, SourceDepthPitch);
		checkf(ValidationResult,
			"Invalid RHI volume texture upload: {}", ToString(ValidationResult.error()));
#endif

		auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto& FormatInfo = GetPixelFormatInfo(VulkanTexture->GetFormat());
		const auto Layout = ComputeTextureUploadLayout(VulkanTexture->GetFormat(),
			UpdateRegion, SourceRowPitch, SourceDepthPitch, SourceData.size());
		requiref(Layout, "Invalid Vulkan volume upload source: {}", ToString(Layout.error()));
		const uint32 DataSize = Layout->Size;
		const uint64 Alignment = std::max<uint64>({16, FormatInfo.BytesPerBlock,
			Device->GetGpuProperties().limits.nonCoherentAtomSize,
			Device->GetGpuProperties().limits.optimalBufferCopyOffsetAlignment});
		FVulkanTransferRange Staging = Context.AcquireTransferRange(
			EVulkanAllocationClassCandidate::TransferUpload, DataSize, Alignment);
		FVulkanBuffer* StagingBuffer = Staging.GetBuffer();
		GVulkanMemoryBaselineTracker.RecordUpload(DataSize);
		auto* Mapped = Staging.GetMappedPointer();
		CopyTextureUploadData(*Layout, SourceData, {Mapped, DataSize});
		Staging.Flush();
		StagingBuffer->GetStateTracker().Apply(
			Staging.GetOffset(), DataSize, ERHIAccess::HostWrite);
		const std::array StagingTransition{FRHIBufferTransition{
			StagingBuffer, Staging.GetOffset(), DataSize,
			ERHIAccess::HostWrite, ERHIAccess::TransferRead}};
		Context.RHITransitionBuffers(StagingTransition);

		const FRHITextureSubresourceRange TransitionRange{
			ERHITextureAspect::Color, MipIndex, 1, 0, 1};
		const ERHIAccess PreviousAccess = VulkanTexture->GetStateTracker().Get(
			ERHITextureAspect::Color, MipIndex, 0);
		const std::array PreCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, PreviousAccess, ERHIAccess::TransferWrite}};
		Context.RHITransitionTextures(PreCopyTransition);
		const std::array CopyRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = Staging.GetOffset(),
			.TextureAspect = ERHITextureAspect::Color,
			.TextureMip = MipIndex,
			.TextureFirstArrayLayer = 0,
			.TextureNumArrayLayers = 1,
			.TextureOffset = {static_cast<int32>(UpdateRegion.DestX),
				static_cast<int32>(UpdateRegion.DestY), static_cast<int32>(UpdateRegion.DestZ)},
			.TextureExtent = {UpdateRegion.Width, UpdateRegion.Height, UpdateRegion.Depth}}};
		Context.RHICopyBufferToTexture(StagingBuffer, VulkanTexture, CopyRegions);
		Staging.Retire();
	}

	auto FVulkanDynamicRHI::ReadTexture2D(
		FVulkanCommandListContext& Context,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		FByteBuffer& OutData,
		std::shared_ptr<FRHITextureReadback> AsyncRequest
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
		const uint64 Alignment = std::max<uint64>({16, FormatInfo.BytesPerBlock,
			Device->GetGpuProperties().limits.nonCoherentAtomSize,
			Device->GetGpuProperties().limits.optimalBufferCopyOffsetAlignment});
		FVulkanTransferRange Readback = AsyncRequest
			? Context.TryAcquireReadbackRange(Layout.DataSize, Alignment)
			: Context.AcquireTransferRange(EVulkanAllocationClassCandidate::TransferReadback, Layout.DataSize, Alignment);
		if (!Readback) return false;
		FVulkanBuffer* ReadbackBuffer = Readback.GetBuffer();
		GVulkanMemoryBaselineTracker.RecordReadback(Layout.DataSize);

		const FRHITextureSubresourceRange TransitionRange{
			ERHITextureAspect::Color, MipIndex, 1, ArraySlice, 1};
		const std::array PreCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, OldAccess, ERHIAccess::TransferRead}};
		Context.RHITransitionTextures(PreCopyTransition);
		const std::array ReadbackTransition{FRHIBufferTransition{
			ReadbackBuffer, Readback.GetOffset(), Layout.DataSize,
			ERHIAccess::Discard, ERHIAccess::TransferWrite}};
		Context.RHITransitionBuffers(ReadbackTransition);
		const std::array CopyRegions{FRHIBufferTextureCopyRegion{
			.BufferOffset = Readback.GetOffset(),
			.TextureAspect = ERHITextureAspect::Color,
			.TextureMip = MipIndex,
			.TextureFirstArrayLayer = ArraySlice,
			.TextureNumArrayLayers = 1,
			.TextureExtent = {Width, Height, 1}}};
		Context.RHICopyTextureToBuffer(VulkanTexture, ReadbackBuffer, CopyRegions);

		const std::array PostCopyTransition{FRHITextureTransition{
			VulkanTexture, TransitionRange, ERHIAccess::TransferRead, OldAccess}};
		Context.RHITransitionTextures(PostCopyTransition);
		const std::array HostTransition{FRHIBufferTransition{
			ReadbackBuffer, Readback.GetOffset(), Layout.DataSize,
			ERHIAccess::TransferWrite, ERHIAccess::HostRead}};
		Context.RHITransitionBuffers(HostTransition);

		const auto ProducingSyncPoint = Device->GetSubmissionCoordinator().SubmitContext(Context);
		check(FRHIGPUSyncPointBackend::GetPoint(ProducingSyncPoint) == FRHIGPUSyncPointBackend::GetPoint(Readback.GetSyncPoint()));
		if (AsyncRequest)
		{
			Context.RetainReadback(std::move(Readback), std::move(AsyncRequest));
			return true;
		}
		FRHIRetirementPrerequisites ReadbackUses;
		require(ReadbackUses.Add(Readback.GetSyncPoint()));
		Device->WaitForUses(ReadbackUses);
		Readback.Invalidate();
		const auto* MappedData = Readback.GetMappedPointer();
		if (MappedData == nullptr)
		{
			DURIN_ERROR("Failed to read Vulkan texture: readback allocation is not mapped.");
			return false;
		}
		const auto* MappedBytes = reinterpret_cast<const std::byte*>(MappedData);
		OutData.assign(MappedBytes, MappedBytes + Layout.DataSize);
		Readback.Retire();
		Device->GetReadbackArena().ReclaimCompleted();
		return true;
	}
} // namespace Durin::VulkanRHI

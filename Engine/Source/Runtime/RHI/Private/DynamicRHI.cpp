#include "DynamicRHI.h"

#include "RHICommandList.h"

namespace Durin
{
	auto FDynamicRHI::RHICreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> TRefCountPtr<FRHIBufferView>
	{
		std::string Error;
		if (!ValidateBufferViewDesc(Buffer, Desc, Error)) return nullptr;
		return new FRHIBufferView(Buffer, Desc);
	}

	auto FDynamicRHI::RHICreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> TRefCountPtr<FRHITextureView>
	{
		std::string Error;
		if (!ValidateTextureViewDesc(Texture, Desc, Error)) return nullptr;
		return new FRHITextureView(Texture, Desc);
	}

	FDynamicRHI* GDynamicRHI = nullptr;

	auto FDynamicRHI::RHIGetCapabilities() const -> const FRHICapabilities*
	{
		return Capabilities ? &*Capabilities : nullptr;
	}

	auto FDynamicRHI::RHIGetGraphicsCacheStatistics() const -> FRHIGraphicsCacheStatistics
	{
		return {};
	}

	auto FDynamicRHI::RHIResetGraphicsCacheStatistics() -> void
	{
	}

	auto FDynamicRHI::RHIGetMemoryStatistics() const -> FRHIMemoryStatistics
	{
		return {};
	}

	auto FDynamicRHI::RHIResetMemoryStatistics() -> void
	{
	}

	auto FDynamicRHI::PublishCapabilities(FRHICapabilities InCapabilities) -> void
	{
		check(!Capabilities.has_value());
		check(InCapabilities.SupportedTextureDimensions != ERHITextureDimensionFlags::None);
		check(InCapabilities.MaxTextureDimension2D > 0);
		check(InCapabilities.MaxTextureDimensionCube > 0);
		check(InCapabilities.MaxTextureArrayLayers >= TextureCubeFaceCount);
		check(InCapabilities.ColorSampleCounts != ERHISampleCountFlags::None);
		check(InCapabilities.DepthSampleCounts != ERHISampleCountFlags::None);
		Capabilities.emplace(std::move(InCapabilities));
	}

	auto FDynamicRHI::ClearCapabilities() -> void
	{
		Capabilities.reset();
	}

	auto FDynamicRHI::RHIUpdateTextureReference(
		FRHITextureReference* TextureReference,
		FRHITexture* NewTexture) -> void
	{
		check(TextureReference != nullptr);
		TextureReference->SetReferencedTexture_RenderThread(NewTexture);
	}

	auto FDynamicRHI::RHIBeginFrame_RenderThread(
		FRHICommandListImmediate& RHICmdList) -> void
	{
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::BeginFrame);
	}

	auto FDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::EndFrame | ERHISubmitFlags::DeleteResources);
	}

	auto FDynamicRHI::RHIAllocateDynamicUniformBuffer(
		FRHICommandListImmediate& RHICmdList,
		const void* Data,
		uint32 Size) -> FRHIUniformBufferRange
	{
		return RHICmdList.AllocateDynamicUniformBufferSynchronous(Data, Size);
	}

	auto FDynamicRHI::RHIAllocateDynamicStorageBuffer(
		FRHICommandListImmediate& RHICmdList,
		const void* Data,
		uint32 Size) -> FRHIStorageBufferRange
	{
		return RHICmdList.AllocateDynamicStorageBufferSynchronous(Data, Size);
	}

	auto FDynamicRHI::RHILockBuffer(
		FRHICommandListImmediate& RHICmdList,
		FRHIBuffer* Buffer,
		uint32 Offset,
		uint32 Size,
		EResourceLockMode LockMode) -> void*
	{
		return RHICmdList.LockBuffer(Buffer, Offset, Size, LockMode);
	}

	auto FDynamicRHI::RHIUnlockBuffer(
		FRHICommandListImmediate& RHICmdList,
		FRHIBuffer* Buffer) -> void
	{
		RHICmdList.UnlockBuffer(Buffer);
	}

	auto FDynamicRHI::RHIUpdateTexture2D(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		const uint8* SourceData) -> void
	{
		RHICmdList.UpdateTexture2D(
			Texture, MipIndex, ArraySlice, UpdateRegion, SourcePitch, SourceData);
	}

	auto FDynamicRHI::RHIReadTexture2D(
		FRHICommandListImmediate& RHICmdList,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		std::vector<uint8>& OutData) -> bool
	{
		return RHICmdList.ReadTexture2D(
			Texture, MipIndex, ArraySlice, OutData);
	}

	auto FDynamicRHI::RHIBlockUntilGPUIdle() -> void
	{
		FRHICommandListImmediate::Get().BlockUntilGPUIdle();
	}
} // namespace Durin

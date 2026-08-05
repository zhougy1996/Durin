#include "DynamicRHI.h"

#include "RHICommandList.h"

namespace Durin
{
	FDynamicRHI* GDynamicRHI = nullptr;

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

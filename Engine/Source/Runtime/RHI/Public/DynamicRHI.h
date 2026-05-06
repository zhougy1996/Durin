#pragma once

#include "RHIAPI.h"
#include "RHIFwd.h"
#include "PixelFormat.h"
#include "RHIResources.h"

namespace Doge
{
	class FDynamicRHI
	{
	public:
		FDynamicRHI() = default;

		virtual ~FDynamicRHI() = default;

		virtual auto Init() -> void = 0;
		virtual auto Shutdown() -> void = 0;

		virtual auto RHIBeginFrame() -> void = 0;
		virtual auto RHIEndFrame() -> void = 0;
		RHI_API virtual auto RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void;

		// Must be called from the main thread.
		virtual auto RHICreateViewport(void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen, EPixelFormat InPreferredPixelFormat) const -> TRefCountPtr<FRHIViewport> = 0;
		// Must be called from the main thread.
		virtual auto RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bIsFullscreen) -> void = 0;

		virtual auto RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> = 0;
		virtual auto RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState> = 0;
		virtual auto RHIGetDefaultContext() -> IRHICommandContext* = 0;
		virtual auto RHIGetViewportBackBuffer(FRHIViewport* InViewportRHI) -> TRefCountPtr<FRHITexture> = 0;

		virtual auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration> = 0;
		virtual auto RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture> = 0;
		virtual auto RHICreateShader(const FRHIShaderCreateDesc& CreateDesc) -> TRefCountPtr<FRHIShader> = 0;
		virtual auto RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer> = 0;
		virtual auto RHILockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void* = 0;
		virtual auto RHIUnlockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer) -> void = 0;
		virtual auto RHIUpdateTexture2D(FRHICommandListBase& RHICmdList, FRHITexture* Texture, uint32 MipIndex, const void* Data, uint32 DataSize, uint32 RowPitch) -> void = 0;

		virtual auto RHIBlockUntilGPUIdle() -> void = 0;
	};

	extern RHI_API FDynamicRHI* GDynamicRHI;

	class IDynamicRHIModule : public IModuleInterface
	{
	public:
		RHI_API virtual auto CreateRHI() -> FDynamicRHI* = 0;
	};

	template<typename TRHI>
	FORCEINLINE auto CastDynamicRHI(FDynamicRHI* InDynamicRHI) -> TRHI*
	{
		return static_cast<TRHI*>(InDynamicRHI);
	}

	template<typename TRHI>
	FORCEINLINE auto GetDynamicRHI() -> TRHI*
	{
		return CastDynamicRHI<TRHI>(GDynamicRHI);
	}
}
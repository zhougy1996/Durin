#pragma once

#include "RHIAPI.h"
#include "RHIFwd.h"
#include "Misc/ViewportPresentModePolicy.h"
#include "PixelFormat.h"
#include "RHIResources.h"
#include "RHICapabilities.h"

namespace Durin
{
	// Reports one bounded cache's current occupancy and lifetime counters.
	struct FRHICacheStatistics
	{
		uint64 Capacity = 0;
		uint64 Occupancy = 0;
		uint64 Hits = 0;
		uint64 Misses = 0;
		uint64 NativeCreations = 0;
		uint64 Evictions = 0;
		uint64 FailedCandidates = 0;
	};

	// Snapshots graphics cache, descriptor-pool, and persistence behavior without a GPU wait.
	struct FRHIGraphicsCacheStatistics
	{
		FRHICacheStatistics DescriptorSnapshots;
		FRHICacheStatistics StructuralLayouts;
		FRHICacheStatistics GraphicsPipelines;
		uint64 DescriptorValueCapacity = 0;
		uint64 DescriptorValueOccupancy = 0;
		uint64 DescriptorAllocations = 0;
		uint64 DescriptorPoolExpansions = 0;
		uint64 PersistentLoads = 0;
		uint64 PersistentSaves = 0;
		uint64 PersistentRejects = 0;
		uint64 PersistentBytes = 0;
	};

	// Defines the backend-neutral device interface used to create resources and submit frame work.
	class FDynamicRHI
	{
	public:
		FDynamicRHI() = default;

		virtual ~FDynamicRHI() = default;

		virtual auto Init() -> void = 0;
		virtual auto Shutdown() -> void = 0;
		RHI_API auto RHIGetCapabilities() const -> const FRHICapabilities*;
		// Counters accumulate for the device lifetime until explicitly reset.
		RHI_API virtual auto RHIGetGraphicsCacheStatistics() const -> FRHIGraphicsCacheStatistics;
		// Clears accumulated counters while preserving capacities and live occupancy.
		RHI_API virtual auto RHIResetGraphicsCacheStatistics() -> void;

		virtual auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void = 0;
		RHI_API virtual auto RHIBeginFrame_RenderThread(
			FRHICommandListImmediate& RHICmdList) -> void;
		virtual auto RHIEndFrame() -> void = 0;
		RHI_API virtual auto RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void;

		// Must be called from the main thread.
		virtual auto RHICreateViewport(void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen, EPixelFormat InPreferredPixelFormat, EViewportPresentModePolicy InPresentModePolicy) const -> TRefCountPtr<FRHIViewport> = 0;
		// Must be called from the main thread.
		virtual auto RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bIsFullscreen) -> void = 0;

		virtual auto RHICreateGraphicsPipelineState(FName DebugName, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> = 0;
		virtual auto RHIGetDefaultContext() -> IRHICommandContext* = 0;
		virtual auto RHIGetViewportBackBuffer(FRHIViewport* InViewportRHI) -> TRefCountPtr<FRHITexture> = 0;

		virtual auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration> = 0;
		// Checks the exact format and usage contract without allocating a resource.
		virtual auto RHIIsTextureSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool = 0;
		virtual auto RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> TRefCountPtr<FRHITexture> = 0;
		// Updates a stable texture identity. Backends may override this to update
		// descriptor or bindless state together with the referenced allocation.
		RHI_API virtual auto RHIUpdateTextureReference(
			FRHITextureReference* TextureReference,
			FRHITexture* NewTexture) -> void;
		virtual auto RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler> = 0;
		virtual auto RHICreateShader(const FRHIShaderCreateDesc& CreateDesc) -> TRefCountPtr<FRHIShader> = 0;
		virtual auto RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer> = 0;
		// Creates one complete immutable view or returns null after a recoverable diagnostic.
		RHI_API virtual auto RHICreateBufferView(
			FRHIBuffer* Buffer,
			const FRHIBufferViewDesc& Desc) -> TRefCountPtr<FRHIBufferView>;
		// Creates one complete immutable view or returns null after a recoverable diagnostic.
		RHI_API virtual auto RHICreateTextureView(
			FRHITexture* Texture,
			const FRHITextureViewDesc& Desc) -> TRefCountPtr<FRHITextureView>;
		RHI_API virtual auto RHIAllocateDynamicUniformBuffer(FRHICommandListImmediate& RHICmdList, const void* Data, uint32 Size) -> FRHIUniformBufferRange;
		RHI_API virtual auto RHIAllocateDynamicStorageBuffer(
			FRHICommandListImmediate& RHICmdList, const void* Data, uint32 Size)
			-> FRHIStorageBufferRange;
		RHI_API auto RHILockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*;
		RHI_API auto RHIUnlockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer) -> void;
		RHI_API auto RHIUpdateTexture2D(FRHICommandListBase& RHICmdList, FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData) -> void;
		// Synchronizes submitted work and returns one tightly packed subresource.
		RHI_API virtual auto RHIReadTexture2D(
			FRHICommandListImmediate& RHICmdList,
			FRHITexture* Texture,
			uint32 MipIndex,
			uint32 ArraySlice,
			std::vector<uint8>& OutData
		) -> bool;

		RHI_API auto RHIBlockUntilGPUIdle() -> void;

	protected:
		RHI_API auto PublishCapabilities(FRHICapabilities InCapabilities) -> void;
		RHI_API auto ClearCapabilities() -> void;

	private:
		std::optional<FRHICapabilities> Capabilities;
	};

	extern RHI_API FDynamicRHI* GDynamicRHI;

	// Creates the platform RHI implementation selected during runtime startup.
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

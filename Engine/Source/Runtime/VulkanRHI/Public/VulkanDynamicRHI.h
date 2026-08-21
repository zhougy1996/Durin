#pragma once

#include "RHI.h"

#include "VulkanExtensions.h"
#include "VulkanDiagnostics.h"

namespace Durin::VulkanRHI
{
	class FVulkanPresentationCandidate;
	class FVulkanDevice;
	class FVulkanViewport;
	class FVulkanCommandListContext;
	class FVulkanViewCache;

	// Extends the portable RHI with native Vulkan handles needed by Vulkan-aware integrations.
	class IVulkanDynamicRHI : public FDynamicRHI
	{
	public:
		virtual auto RHIGetVkDevice() const -> vk::Device = 0;
		virtual auto RHIGetVkInstance() const -> vk::Instance = 0;
		virtual auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice = 0;
		// Runs a bounded native integration callback on the current command
		// buffer's owning thread. The handle is borrowed for this call only.
		virtual auto RHIExecuteCommandBufferForBackendIntegration(
			std::function<void(vk::CommandBuffer)> Operation) -> void = 0;

	};

	// Owns the Vulkan instance and device and implements the backend-neutral RHI contract.
	class FVulkanDynamicRHI final: public IVulkanDynamicRHI
	{
	public:
		FVulkanDynamicRHI();
		~FVulkanDynamicRHI() override;

		static auto Get() -> FVulkanDynamicRHI& { return *GetDynamicRHI<FVulkanDynamicRHI>(); }

		auto Init(const FRHIInitializationContext& Context) -> void override;
		auto Shutdown() -> void override;

		auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override;
		auto RHIBeginFrame_RenderThread(
			FRHICommandListImmediate& RHICmdList) -> void override;
		auto RHIEndFrame() -> void override;
		auto RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void override;

		auto RHIGetVkDevice() const -> vk::Device override;
		auto RHIGetVkInstance() const -> vk::Instance override;
		auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice override;
		auto RHIExecuteCommandBufferForBackendIntegration(
			std::function<void(vk::CommandBuffer)> Operation) -> void override;
		auto IsInstanceExtensionEnabled(const char* ExtensionName) const -> bool;
		auto GetDiagnosticAvailability() const
			-> const FVulkanDiagnosticAvailability& { return DiagnosticAvailability; }
		auto GetDebugMessageStatistics() const
			-> FVulkanDebugMessageStatistics { return DebugCallbackState.Snapshot(); }
		auto GetDebugUtils() -> FVulkanDebugUtils& { return DebugUtils; }

		auto RHICreateViewport(const FRHIViewportCreateInfo& CreateInfo)
			-> FViewportRHIRef override;
		auto RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void override;
		auto RHICreateGraphicsPipelineState(FName DebugName, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> override;
		auto RHICreateComputePipelineState(FName DebugName,
			const FComputePipelineStateInitializer& Initializer)
			-> TRefCountPtr<FRHIComputePipelineState> override;
		auto RHICreateGPUTimingQuery() -> TRefCountPtr<FRHIGPUTimingQuery> override;
		auto RHIGetGPUTimingResult(const FRHIGPUTimingQuery* Query) const
			-> FRHIGPUTimingResult override;
		auto RHIGetGraphicsCacheStatistics() const -> FRHIGraphicsCacheStatistics override;
		auto RHIResetGraphicsCacheStatistics() -> void override;
		auto RHIGetMemoryStatistics() const -> FRHIMemoryStatistics override;
		auto RHIResetMemoryStatistics() -> void override;
		auto RHIGetDiagnosticSnapshot() const -> FRHIDiagnosticSnapshot override;
		auto RHIResetDiagnosticStatistics() -> void override;
		auto RHIGetDefaultContext() -> IRHICommandContext* override;
		auto RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*;
		auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> FTextureRHIRef override;

		auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration> override;
		auto RHIIsTextureSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool override;
		auto RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> FTextureRHIRef override;
		auto RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler> override;
		auto RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> FBufferRHIRef override;
		auto RHICreateBufferView(FRHIBuffer* Buffer,
			const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef override;
		auto RHICreateTextureView(FRHITexture* Texture,
			const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef override;
		auto RHIGetOrCreateBufferView(FRHIBuffer* Buffer,
			const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef override;
		auto RHIGetOrCreateTextureView(FRHITexture* Texture,
			const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef override;
		auto RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef override;
		auto RHIAllocateDynamicUniformBuffer(
			FRHICommandListImmediate& RHICmdList,
			const void* Data,
			uint32 Size) -> FRHIUniformBufferRange override;
		auto RHIAllocateDynamicStorageBuffer(
			FRHICommandListImmediate& RHICmdList,
			const void* Data,
			uint32 Size) -> FRHIStorageBufferRange override;

		// Internal Vulkan helpers use the device for diagnostics in every runtime variant;
		// failure-injection tests share the same non-owning access.
		auto GetDeviceForTesting() const -> FVulkanDevice* { return Device; }

		auto InitializeTexture(FVulkanCommandListContext& Context, FRHITexture* Texture) -> void;
		auto UpdateTexture2D(
			FVulkanCommandListContext& Context,
			FRHITexture* Texture,
			uint32 MipIndex,
			uint32 ArraySlice,
			const FUpdateTextureRegion2D& UpdateRegion,
			uint32 SourcePitch,
			std::span<const uint8> SourceData) -> void;
		auto UpdateTexture3D(
			FVulkanCommandListContext& Context,
			FRHITexture* Texture,
			uint32 MipIndex,
			const FUpdateTextureRegion3D& UpdateRegion,
			uint32 SourceRowPitch,
			uint32 SourceDepthPitch,
			std::span<const uint8> SourceData) -> void;
		auto ReadTexture2D(
			FVulkanCommandListContext& Context,
			FRHITexture* Texture,
			uint32 MipIndex,
			uint32 ArraySlice,
			std::vector<uint8>& OutData
		) -> bool;

	protected:
		auto CreateInstance() -> void;
		auto CreateDebugMessenger() -> void;
		auto DestroyDebugMessenger() -> void;
		auto SelectDevice(
			vk::SurfaceKHR PresentationSurface,
			bool bRequirePresentation) -> void;

	private:
		vk::Instance Instance;
		vk::DebugUtilsMessengerEXT DebugMessenger;
		std::vector<std::string> InstanceExtensions;
		std::vector<std::string> InstanceLayers;
		FVulkanDiagnosticAvailability DiagnosticAvailability;
		FVulkanDebugCallbackState DebugCallbackState;
		FVulkanDebugUtils DebugUtils;

		FVulkanDevice* Device = nullptr;
		std::unique_ptr<FVulkanViewCache> ViewCache;
		std::unique_ptr<FVulkanPresentationCandidate>
			InitializationPresentationCandidate;
	};

	extern FVulkanDynamicRHI* GVulkanRHI;

	// Creates and publishes the Vulkan RHI implementation during module startup.
	class FVulkanDynamicRHIModule : public IDynamicRHIModule
	{
	public:
		auto CreateRHI() -> FDynamicRHI* override
		{
			GVulkanRHI = new FVulkanDynamicRHI();
			return GVulkanRHI;
		}
	};

}

#pragma once

#include "RHI.h"

#include "VulkanExtensions.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanViewport;
	class FVulkanCommandListContext;

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

		auto Init() -> void override;
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

		auto RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat, EViewportPresentModePolicy InPresentModePolicy) const -> FViewportRHIRef override;
		auto RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void override;
		auto RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> override;
		auto RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState> override;
		auto RHIGetDefaultContext() -> IRHICommandContext* override;
		auto RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*;
		auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> FTextureRHIRef override;

		auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration> override;
		auto RHIIsTextureFormatSupported(const FRHITextureCreateDesc& CreateDesc) const -> bool override;
		auto RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> FTextureRHIRef override;
		auto RHICreateSampler(const FRHISamplerDesc& CreateDesc) -> TRefCountPtr<FRHISampler> override;
		auto RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> FBufferRHIRef override;
		auto RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef override;
		auto RHIAllocateDynamicUniformBuffer(
			FRHICommandListImmediate& RHICmdList,
			const void* Data,
			uint32 Size) -> FRHIUniformBufferRange override;

		auto InitializeTexture(FVulkanCommandListContext& Context, FRHITexture* Texture) -> void;
		auto UpdateTexture2D(
			FVulkanCommandListContext& Context,
			FRHITexture* Texture,
			uint32 MipIndex,
			uint32 ArraySlice,
			const FUpdateTextureRegion2D& UpdateRegion,
			uint32 SourcePitch,
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
		auto SelectDevice() -> void;

		auto SetupInstanceLayers(const FVulkanInstanceExtensionArray& DurinExtensions) -> void;

	private:
		vk::Instance Instance;
		std::vector<const char*> InstanceExtensions;
		std::vector<const char*> InstanceLayers;

		FVulkanDevice* Device = nullptr;
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

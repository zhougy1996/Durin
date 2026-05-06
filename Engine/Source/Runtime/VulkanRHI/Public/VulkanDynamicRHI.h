#pragma once

#include "RHI.h"

#include "VulkanExtensions.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanViewport;
	class FVulkanCommandListContext;

	class IVulkanDynamicRHI : public FDynamicRHI
	{
	public:
		virtual auto RHIGetVkDevice() const -> vk::Device = 0;
		virtual auto RHIGetVkInstance() const -> vk::Instance = 0;
		virtual auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice = 0;
	};

	class FVulkanDynamicRHI final: public IVulkanDynamicRHI
	{
	public:
		FVulkanDynamicRHI();
		~FVulkanDynamicRHI() override = default;

		static auto Get() -> FVulkanDynamicRHI& { return *GetDynamicRHI<FVulkanDynamicRHI>(); }

		auto Init() -> void override;
		auto Shutdown() -> void override;

		auto RHIBeginFrame() -> void override;
		auto RHIEndFrame() -> void override;
		auto RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void override;

		auto RHIGetVkDevice() const -> vk::Device override;
		auto RHIGetDynamicLoader() -> vk::DynamicLoader& { return DynamicLoader; }
		auto RHIGetVkInstance() const -> vk::Instance override;
		auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice override;

		auto RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> FViewportRHIRef override;
		auto RHIResizeViewport(FRHIViewport* InViewport, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen) -> void override;
		auto RHICreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> override;
		auto RHIGetGraphicsPipelineState(FName Name) -> TRefCountPtr<FRHIGraphicsPipelineState> override;
		auto RHIGetDefaultContext() -> IRHICommandContext* override;
		auto RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*;
		auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> FTextureRHIRef override;

		auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> TRefCountPtr<FRHIVertexDeclaration> override;
		auto RHICreateTexture(FRHICommandListBase& RHICmdList, const FRHITextureCreateDesc& CreateDesc) -> FTextureRHIRef override;
		auto RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> FBufferRHIRef override;
		auto RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef override;
		auto RHILockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void* override;
		auto RHIUnlockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer) -> void override;
		auto RHIUpdateTexture2D(FRHICommandListBase& RHICmdList, FRHITexture* Texture, uint32 MipIndex, const void* Data, uint32 DataSize, uint32 RowPitch) -> void override;

		auto RHIBlockUntilGPUIdle() -> void override;

	protected:
		auto CreateInstance() -> void;
		auto SelectDevice() -> void;

		auto SetupInstanceLayers(const FVulkanInstanceExtensionArray& DogeExtensions) -> void;

	private:
		vk::DynamicLoader DynamicLoader{};
		vk::Instance Instance;
		std::vector<const char*> InstanceExtensions;
		std::vector<const char*> InstanceLayers;

		FVulkanDevice* Device = nullptr;
	};

	extern FVulkanDynamicRHI* GVulkanRHI;

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

#pragma once

#include "RHI.h"

class FVulkanDevice;
class FVulkanViewport;
class FVulkanCommandListContext;

class VULKAN_RHI_API IVulkanDynamicRHI : public IDynamicRHI
{
public:
	IVulkanDynamicRHI() = default;

	virtual auto RHIGetVkDevice() const -> vk::Device = 0;
	virtual auto RHIGetVkInstance() const -> vk::Instance = 0;
	virtual auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice = 0;

	virtual auto RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TSharedPtr<FRHIViewport> override = 0;
	virtual auto RHICreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FRHIGraphicsPipelineState> override = 0;
	virtual auto RHIGetDefaultContext() -> IRHICommandContext* override = 0;
	virtual auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TSharedPtr<FRHITexture> override = 0;
};

class VULKAN_RHI_API FVulkanDynamicRHI : public IVulkanDynamicRHI
{
public:
	FVulkanDynamicRHI();
	~FVulkanDynamicRHI() = default;

	static auto Get() -> FVulkanDynamicRHI& { return *GetDynamicRHI<FVulkanDynamicRHI>(); }

	auto Init() -> void override;
	auto Shutdown() -> void override;

	auto RHIGetVkDevice() const -> vk::Device override;
	auto RHIGetVkInstance() const -> vk::Instance override;
	auto RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice override;

	virtual auto RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TSharedPtr<FRHIViewport> override;
	virtual auto RHICreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FRHIGraphicsPipelineState> override;
	virtual auto RHIGetDefaultContext() -> IRHICommandContext* override;
	virtual auto RHIGetCommandContext(ERHIPipeline Pipeline) -> IRHICommandContext*;
	virtual auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TSharedPtr<FRHITexture> override;

protected:
	auto CreateInstance() -> void;
	auto SelectDevice() -> void;

	auto SetupInstanceLayers(const FVulkanInstanceExtensionArray& DogeExtensions) -> void;

private:
	vk::Instance Instance_;
	TArray<const char*> InstanceExtensions_;
	TArray<const char*> InstanceLayers_;

	FVulkanDevice* Device_ = nullptr;
};

FVulkanDynamicRHI* GVulkanRHI = nullptr;

class VULKAN_RHI_API FVulkanDynamicRHIModule : public IDynamicRHIModule
{
public:
	auto CreateRHI() -> IDynamicRHI* override
	{
		GVulkanRHI = new FVulkanDynamicRHI();
		return GVulkanRHI;
	}
};

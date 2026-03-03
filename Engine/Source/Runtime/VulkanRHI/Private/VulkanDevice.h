#pragma once

#include "RHIDefinitions.h"
#include "VulkanMemory.h"
#include "VulkanExtensions.h"

namespace Doge::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanQueue;
	class FVulkanCommandListContext;
	class FVulkanRenderPassManager;
	class FVulkanPipelineManager;

	class FVulkanDevice
	{
	public:
		FVulkanDevice(FVulkanDynamicRHI* InRHI, vk::PhysicalDevice InGpu);

		~FVulkanDevice();

		auto InitGpu() -> void;

		auto CreateDevice(const FVulkanDeviceExtensionArray& InDeviceExtensions) -> void;

		auto SetupPresentQueue(vk::SurfaceKHR InSurface) -> void;

		auto WaitUtilIdle() -> void;

		auto GetHandle() const -> vk::Device;

		auto GetGpu() const -> vk::PhysicalDevice;

		auto GetRenderPassManager() -> FVulkanRenderPassManager&;

		auto AcquireDeferredContext() -> FVulkanCommandListContext*;

		auto GetImmediateContext() const -> FVulkanCommandListContext* { return ImmediateContext; }

		auto ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void;

		auto GetPresentQueue() const -> FVulkanQueue* { return PresentQueue; }

		auto GetGraphicsQueue() const -> FVulkanQueue* { return GraphicsQueue; }

		auto GetFenceManager() -> FVulkanFenceManager& { return FenceManager; }

		auto GetPipelineManager() const -> FVulkanPipelineManager& { return *PipelineManager; }

	private:
		auto Destroy() -> void;

		FVulkanDynamicRHI* RHI;

		vk::Device Device;

		vk::PhysicalDevice Gpu;

		vk::PhysicalDeviceProperties GpuProps;

		std::vector<vk::QueueFamilyProperties> QueueFamilyProps;

		FVulkanFenceManager FenceManager;

		FVulkanRenderPassManager* RenderPassManager;

		FVulkanPipelineManager* PipelineManager;

		FVulkanQueue* GraphicsQueue = nullptr;

		FVulkanQueue* ComputeQueue = nullptr;

		FVulkanQueue* TransferQueue = nullptr;

		FVulkanQueue* PresentQueue = nullptr;

		std::vector<const char*> DeviceExtensions;

		std::vector<FVulkanCommandListContext*> CommandContexts;

		FVulkanCommandListContext* ImmediateContext = nullptr;

		EGpuVendorId VendorId = EGpuVendorId::Unknown;
	};
}
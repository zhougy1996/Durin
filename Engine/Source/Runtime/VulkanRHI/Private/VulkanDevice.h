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
		FVulkanDevice(FVulkanDynamicRHI* RHI, vk::PhysicalDevice Gpu);

		~FVulkanDevice();

		auto InitGpu() -> void;

		auto CreateDevice(FVulkanDeviceExtensionArray& DeviceExtensions) -> void;

		auto SetupPresentQueue(vk::SurfaceKHR Surface) -> void;

		auto WaitUtilIdle() -> void;

		auto GetHandle() const -> vk::Device;

		auto GetGpu() const -> vk::PhysicalDevice;

		auto GetRenderPassManager() -> FVulkanRenderPassManager&;

		auto AcquireDeferredContext() -> FVulkanCommandListContext*;

		auto GetImmediateContext() -> FVulkanCommandListContext* const { return ImmediateContext_; }

		auto ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void;

		auto GetPresentQueue() const -> FVulkanQueue* { return PresentQueue_; }

		auto GetGraphicsQueue() const -> FVulkanQueue* { return GraphicsQueue_; }

		auto GetFenceManager() -> FVulkanFenceManager& { return FenceManager_; }

		auto GetPipelineManager() -> FVulkanPipelineManager& { return *PipelineManager_; }

	private:
		auto Destroy() -> void;

		FVulkanDynamicRHI* RHI_;

		vk::Device Device_;

		vk::PhysicalDevice Gpu_;

		vk::PhysicalDeviceProperties GpuProps_;

		std::vector<vk::QueueFamilyProperties> QueueFamilyProps_;

		FVulkanFenceManager FenceManager_;

		FVulkanRenderPassManager* RenderPassManager_;

		FVulkanPipelineManager* PipelineManager_;

		FVulkanQueue* GraphicsQueue_ = nullptr;

		FVulkanQueue* ComputeQueue_ = nullptr;

		FVulkanQueue* TransferQueue_ = nullptr;

		FVulkanQueue* PresentQueue_ = nullptr;

		std::vector<const char*> DeviceExtensions_;

		std::vector<FVulkanCommandListContext*> CommandContexts_;

		FVulkanCommandListContext* ImmediateContext_ = nullptr;

		EGpuVendorId VendorId_ = EGpuVendorId::Unknown;
	};
}
#pragma once

#include "RHIDefinitions.h"
#include "VulkanMemory.h"
#include "VulkanExtensions.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanDynamicRHI;
	class FVulkanQueue;
	class FVulkanCommandListContext;
	class FVulkanRenderPassManager;
	class FVulkanPipelineManager;

	extern uint64 GVulkanRHIDeletionFrameNumber;

	class FDeferredDeletionQueue
	{
	public:
		enum class EType
		{
			RenderPass,
			Buffer,
			BufferView,
			Image,
			ImageView,
			Pipeline,
			PipelineLayout,
			Framebuffer,
			DescriptorSetLayout,
			Sampler,
			Semaphore,
			ShaderModule,
			Event,
			ResourceAllocation,
			DeviceMemoryAllocation,
			BufferSuballocation,
			AccelerationStructure,
			BindlessHandle,
		};

		explicit FDeferredDeletionQueue(FVulkanDevice* InDevice);

		template<typename T>
		auto EnqueueResource(EType Type, T Handle) -> void
		{
			static_assert(sizeof(T) <= sizeof(uint64), "Vulkan resource handle type size too large.");
			EnqueueGenericResource(Type, static_cast<uint64>(Handle));
		}

		auto ReleaseResources(bool bDeleteImmediately = false) -> void;

	private:
		struct FEntry
		{
			EType Type;
			uint64 FrameNumber;
			uint64 Handle;
		};

		auto EnqueueGenericResource(EType Type, uint64 Handle) -> void;

		auto ReleaseResourceImmediately(const std::vector<FEntry>& InEntries) const -> void;

		FVulkanDevice* Device;

		std::mutex Mutex;
		std::vector<FEntry> Entries;
	};

	class FVulkanDevice
	{
	public:
		FVulkanDevice(FVulkanDynamicRHI* InRHI, vk::PhysicalDevice InGpu);

		~FVulkanDevice();

		auto InitGpu() -> void;

		auto CreateDevice(const FVulkanDeviceExtensionArray& InDeviceExtensions) -> void;

		auto SetupPresentQueue(vk::SurfaceKHR InSurface) -> void;

		auto WaitUtilIdle() const -> void;

		auto GetHandle() const -> vk::Device;

		auto GetGpu() const -> vk::PhysicalDevice;

		auto GetRenderPassManager() const -> FVulkanRenderPassManager&;

		auto AcquireDeferredContext() -> FVulkanCommandListContext*;

		auto GetImmediateContext() const -> FVulkanCommandListContext* { return ImmediateContext; }

		auto ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void;

		auto GetPresentQueue() const -> FVulkanQueue* { return PresentQueue; }

		auto GetGraphicsQueue() const -> FVulkanQueue* { return GraphicsQueue; }

		auto GetFenceManager() -> FVulkanFenceManager& { return FenceManager; }

		auto GetPipelineManager() const -> FVulkanPipelineManager& { return *PipelineManager; }

		auto GetDeferredDeletionQueue() -> FDeferredDeletionQueue& { return DeferredDeletionQueue; }

	private:
		auto Destroy() -> void;

		FVulkanDynamicRHI* RHI;

		vk::Device Device;

		vk::PhysicalDevice Gpu;

		vk::PhysicalDeviceProperties GpuProps;

		std::vector<vk::QueueFamilyProperties> QueueFamilyProps;

		FVulkanFenceManager FenceManager;

		FVulkanRenderPassManager* RenderPassManager = nullptr;

		FVulkanPipelineManager* PipelineManager = nullptr;

		FVulkanQueue* GraphicsQueue = nullptr;

		FVulkanQueue* ComputeQueue = nullptr;

		FVulkanQueue* TransferQueue = nullptr;

		FVulkanQueue* PresentQueue = nullptr;

		std::vector<const char*> DeviceExtensions;

		std::vector<FVulkanCommandListContext*> CommandContexts;

		FVulkanCommandListContext* ImmediateContext = nullptr;

		EGpuVendorId VendorId = EGpuVendorId::Unknown;

		FDeferredDeletionQueue DeferredDeletionQueue;
	};
} // namespace Doge::VulkanRHI
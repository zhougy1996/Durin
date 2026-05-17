#pragma once

#include "RHIDefinitions.h"
#include "VulkanMemory.h"
#include "VulkanExtensions.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanDynamicRHI;
	class FVulkanQueue;
	class FVulkanCommandListContext;
	class FVulkanRenderPassManager;
	class FVulkanPipelineManager;
	class FVulkanFrame;
	class FVulkanGlobalDescriptorPool;

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
			DescriptorSet,
			DescriptorPool,
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
			// Convert cpp-style handle to c-style handle
			typename T::NativeType RawHandle = static_cast<typename T::NativeType>(Handle);
			EnqueueGenericResource(Type, reinterpret_cast<uint64>(RawHandle));
		}

		template<typename T>
		auto EnqueueResource(EType Type, T Handle, const FVulkanAllocation& Allocation) -> void
		{
			static_assert(sizeof(T) <= sizeof(uint64), "Vulkan resource handle type size too large.");
			// Convert cpp-style handle to c-style handle
			typename T::NativeType RawHandle = static_cast<typename T::NativeType>(Handle);
			EnqueueAllocatedResource(Type, reinterpret_cast<uint64>(RawHandle), Allocation);
		}

		auto ReleaseResources(bool bDeleteImmediately = false) -> void;

		// Called from the game thread to clear the queue,
		// Should only be called when the device is being destroyed at which point the render thread should have already been shut down and all resources should be safe to delete immediately.
		auto Clear() -> void;

	private:
		struct FEntry
		{
			EType Type;
			uint64 FrameNumber;
			uint64 Handle;
			FVulkanAllocation Allocation;
		};

		auto EnqueueGenericResource(EType Type, uint64 Handle) -> void;

		auto EnqueueAllocatedResource(EType Type, uint64 Handle, const FVulkanAllocation& Allocation) -> void;

		auto ReleaseResourceImmediately(std::vector<FEntry>& InEntries) const -> void;

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

		auto GetGpuProperties() const -> const vk::PhysicalDeviceProperties& { return GpuProps; }

		auto GetRenderPassManager() const -> FVulkanRenderPassManager&;

		auto AcquireDeferredContext() -> FVulkanCommandListContext*;

		auto GetImmediateContext() const -> FVulkanCommandListContext* { return ImmediateContext; }

		auto ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void;

		auto GetPresentQueue() const -> FVulkanQueue* { return PresentQueue; }

		auto GetGraphicsQueue() const -> FVulkanQueue* { return GraphicsQueue; }

		auto GetMemoryManager() -> FVulkanMemoryManager& { return MemoryManager; }

		auto GetFenceManager() -> FVulkanFenceManager& { return FenceManager; }

		auto GetPipelineManager() const -> FVulkanPipelineManager& { return *PipelineManager; }

		auto GetGlobalDescriptorPool() const -> FVulkanGlobalDescriptorPool& { return *GlobalDescriptorPool; }

		auto GetDeferredDeletionQueue() -> FDeferredDeletionQueue& { return DeferredDeletionQueue; }

		auto GetCurrentFrame() -> FVulkanFrame&;

	private:
		auto Destroy() -> void;

		FVulkanDynamicRHI* RHI;

		vk::Device Device;

		vk::PhysicalDevice Gpu;

		vk::PhysicalDeviceProperties GpuProps;

		std::vector<vk::QueueFamilyProperties> QueueFamilyProps;

		FVulkanMemoryManager MemoryManager;

		FVulkanFenceManager FenceManager;

		FVulkanRenderPassManager* RenderPassManager = nullptr;

		FVulkanPipelineManager* PipelineManager = nullptr;

		FVulkanGlobalDescriptorPool* GlobalDescriptorPool = nullptr;

		std::array<FVulkanFrame*, kFrameInFlight> Frames = {};

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
} // namespace Durin::VulkanRHI
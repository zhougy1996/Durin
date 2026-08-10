#pragma once

#include "RHIDefinitions.h"
#include "VulkanMemory.h"
#include "VulkanExtensions.h"

namespace Durin::VulkanRHI
{
	struct FVulkanQueueFamilyCandidate
	{
		vk::QueueFlags Flags;
		uint32 QueueCount = 0;
		bool bSupportsWin32Presentation = false;
	};

	struct FVulkanPhysicalDeviceCandidateInput
	{
		std::string DeviceName;
		vk::PhysicalDeviceType DeviceType = vk::PhysicalDeviceType::eOther;
		uint32 ApiVersion = 0;
		uint32 VendorId = 0;
		uint32 DeviceId = 0;
		uint32 MaxImageDimension2D = 0;
		uint32 MaxImageDimensionCube = 0;
		uint32 MaxImageArrayLayers = 0;
		bool bFillModeNonSolid = false;
		bool bIndependentBlend = false;
		bool bShaderDrawParameters = false;
		bool bSynchronization2Feature = false;
		bool bSwapchainMaintenanceFeature = false;
		bool bHasSwapchainMaintenanceInstanceDependencies = false;
		std::vector<std::string> AvailableExtensions;
		std::vector<FVulkanQueueFamilyCandidate> QueueFamilies;
	};

	struct FVulkanPhysicalDeviceCandidateEvaluation
	{
		std::vector<std::string> RejectionReasons;
		std::vector<std::string> EnabledExtensions;
		int32 GraphicsPresentQueueFamilyIndex = -1;
		bool bEnableSynchronization2 = false;
		bool bEnableSwapchainMaintenance1 = false;

		auto IsSuitable() const -> bool { return RejectionReasons.empty(); }
	};

	VULKANRHI_API auto EvaluateVulkanPhysicalDeviceCandidate(
		const FVulkanPhysicalDeviceCandidateInput& Input)
		-> FVulkanPhysicalDeviceCandidateEvaluation;
	VULKANRHI_API auto IsVulkanPhysicalDeviceCandidatePreferred(
		const FVulkanPhysicalDeviceCandidateInput& Left,
		const FVulkanPhysicalDeviceCandidateInput& Right) -> bool;
	VULKANRHI_API auto FormatVulkanPhysicalDeviceRejectionDiagnostic(
		std::span<const FVulkanPhysicalDeviceCandidateInput> Inputs,
		std::span<const FVulkanPhysicalDeviceCandidateEvaluation> Evaluations)
		-> std::string;

	class FVulkanDevice;
	class FVulkanDynamicRHI;
	class FVulkanQueue;
	class FVulkanCommandListContext;
	class FVulkanRenderPassManager;
	class FVulkanPipelineManager;
	class FVulkanGraphicsPipelineState;
	class FVulkanFrame;
	class FVulkanGlobalDescriptorPool;
	class FVulkanDescriptorSetLayoutCache;
	class FVulkanDynamicUniformBufferAllocator;
	class FVulkanDynamicStorageBufferAllocator;
	class FVulkanCompletionTracker;
	class FVulkanTransferArena;
	class FVulkanGPUTimingManager;

	// Defers destruction of Vulkan handles until their queue completion token retires.
	class FDeferredDeletionQueue
	{
	public:
		// Identifies the Vulkan handle category stored for deferred destruction.
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
		// Records one handle and the exact queue token required for destruction.
		struct FEntry
		{
			EType Type;
			uint64 CompletionToken;
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

	// Owns the selected physical/logical device and all device-scoped backend services.
	class FVulkanDevice
	{
	public:
		FVulkanDevice(
			FVulkanDynamicRHI* InRHI,
			vk::PhysicalDevice InGpu,
			FVulkanPhysicalDeviceCandidateEvaluation InEvaluation);

		~FVulkanDevice();

		auto InitGpu(uint32 EnabledInstanceExtensionCount) -> void;

		auto CreateDevice() -> void;

		auto SetupPresentQueue(vk::SurfaceKHR InSurface) -> bool;

		auto WaitUtilIdle() const -> void;

		auto GetHandle() const -> vk::Device;
		auto GetRHI() const -> FVulkanDynamicRHI& { return *RHI; }

		auto GetGpu() const -> vk::PhysicalDevice;

		auto GetGpuProperties() const -> const vk::PhysicalDeviceProperties& { return GpuProps; }
		auto GetQueueFamilyProperties(uint32 FamilyIndex) const
			-> const vk::QueueFamilyProperties&
		{
			check(FamilyIndex < QueueFamilyProps.size());
			return QueueFamilyProps[FamilyIndex];
		}

		auto GetRenderPassManager() const -> FVulkanRenderPassManager&;

		auto AcquireDeferredContext() -> FVulkanCommandListContext*;

		auto GetImmediateContext() const -> FVulkanCommandListContext* { return ImmediateContext; }

		auto ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void;

		auto GetPresentQueue() const -> FVulkanQueue* { return PresentQueue; }

		auto SupportsSwapchainMaintenance1() const -> bool { return bSupportsSwapchainMaintenance1; }
		auto SupportsSynchronization2() const -> bool { return bSupportsSynchronization2; }

		auto GetGraphicsQueue() const -> FVulkanQueue* { return GraphicsQueue; }

		auto GetMemoryManager() -> FVulkanMemoryManager& { return MemoryManager; }

		auto GetFenceManager() -> FVulkanFenceManager& { return FenceManager; }
		auto GetCompletionTracker() -> FVulkanCompletionTracker&
		{
			return *CompletionTracker;
		}
		auto GetUploadArena() -> FVulkanTransferArena& { return *UploadArena; }
		auto GetReadbackArena() -> FVulkanTransferArena& { return *ReadbackArena; }
		auto GetGPUTimingManager() -> FVulkanGPUTimingManager& { return *GPUTimingManager; }

		auto GetPipelineManager() const -> FVulkanPipelineManager& { return *PipelineManager; }

		auto GetGlobalDescriptorPool() const -> FVulkanGlobalDescriptorPool& { return *GlobalDescriptorPool; }

		auto GetDescriptorSetLayoutCache() const -> FVulkanDescriptorSetLayoutCache& { return *DescriptorSetCache; }

		auto GetDeferredDeletionQueue() -> FDeferredDeletionQueue& { return DeferredDeletionQueue; }

		auto GetDynamicUniformBufferAllocator() -> FVulkanDynamicUniformBufferAllocator& { return *DynamicUniformBufferAllocator; }
		auto GetDynamicStorageBufferAllocator() -> FVulkanDynamicStorageBufferAllocator& { return *DynamicStorageBufferAllocator; }

		auto GetCurrentFrame() -> FVulkanFrame&;
		auto SetCurrentFrameIndex(uint32 FrameIndex) -> void;
		auto GetCurrentFrameIndex() const -> uint32;
		auto GetGraphicsCacheStatistics() const -> const FRHIGraphicsCacheStatistics& { return GraphicsCacheStatistics; }
		auto GetGraphicsCacheStatisticsMutable() -> FRHIGraphicsCacheStatistics& { return GraphicsCacheStatistics; }
		auto ResetGraphicsCacheStatistics() -> void;

		auto NotifyDeleted_Image(vk::Image Image) -> void;
		auto NotifyDeleted_GraphicsPipeline(
			FVulkanGraphicsPipelineState* PipelineState) -> void;

	private:
		auto Destroy() -> void;

		FVulkanDynamicRHI* RHI;

		vk::Device Device;

		vk::PhysicalDevice Gpu;

		vk::PhysicalDeviceProperties GpuProps;

		std::vector<vk::QueueFamilyProperties> QueueFamilyProps;

		FVulkanMemoryManager MemoryManager;

		FVulkanFenceManager FenceManager;
		FVulkanCompletionTracker* CompletionTracker = nullptr;
		FVulkanTransferArena* UploadArena = nullptr;
		FVulkanTransferArena* ReadbackArena = nullptr;
		FVulkanGPUTimingManager* GPUTimingManager = nullptr;

		FVulkanRenderPassManager* RenderPassManager = nullptr;

		FVulkanPipelineManager* PipelineManager = nullptr;

		FVulkanGlobalDescriptorPool* GlobalDescriptorPool = nullptr;

		FVulkanDescriptorSetLayoutCache* DescriptorSetCache = nullptr;

		FVulkanDynamicUniformBufferAllocator* DynamicUniformBufferAllocator = nullptr;
		FVulkanDynamicStorageBufferAllocator* DynamicStorageBufferAllocator = nullptr;

		std::array<FVulkanFrame*, kFrameInFlight> Frames = {};
		uint32 CurrentFrameIndex = 0;

		FRHIGraphicsCacheStatistics GraphicsCacheStatistics;

		FVulkanQueue* GraphicsQueue = nullptr;

		FVulkanQueue* ComputeQueue = nullptr;

		FVulkanQueue* TransferQueue = nullptr;

		FVulkanQueue* PresentQueue = nullptr;

		std::vector<std::string> DeviceExtensions;

		bool bSupportsSwapchainMaintenance1 = false;
		bool bSupportsSynchronization2 = false;

		int32 GraphicsQueueFamilyIndex = -1;

		int32 ComputeQueueFamilyIndex = -1;

		int32 TransferQueueFamilyIndex = -1;

		std::vector<FVulkanCommandListContext*> CommandContexts;

		FVulkanCommandListContext* ImmediateContext = nullptr;

		EGpuVendorId VendorId = EGpuVendorId::Unknown;

		FDeferredDeletionQueue DeferredDeletionQueue;
	};
} // namespace Durin::VulkanRHI
